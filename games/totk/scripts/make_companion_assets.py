#!/usr/bin/env python3.14
"""Extract the Android companion's assets from the player's verified 1.4.3 RomFS.

Usage: python3.14 make_companion_assets.py --romfs <romfs.bin/index.json directory> --out <asset root>
Requires Pillow and texture2ddecoder (games/totk/requirements.txt). Output is
<out>/companion; never point --out at a source directory. No game data ships
with this script. Workers read only their current texture from the RomFS.
Add --review-dir <scratch directory> to render static BFLYT references and a
named decor contact sheet. The manifest records decor/palette/pageIcons and
layout/pane provenance; bflyt_render.py documents offline rendering limits.

Map variants prefer all-T (revealed terrain), then the longest A/B suffix
chain (complete sky composites rather than transparent island-only overlays),
then B over A. B is the post-event choice [INFERENCE]; A/B terrain differences
are small and both are authored game maps. Native column/row order and world
bounds align with the static positions of Lookout Landing, Kakariko, Hateno
and Tarrey Town; no runtime coordinate calibration was performed.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import time

try:
    from compression import zstd
except ImportError:
    sys.exit('Companion assets require Python 3.14 (compression.zstd).')
try:
    from PIL import Image, ImageChops
    import texture2ddecoder as decoder
except ImportError:
    sys.exit('Companion assets require Pillow and texture2ddecoder. Run: python3.14 -m pip install -r games/totk/requirements.txt')

from companion_decor import extract_decor

# The APK generates the same tree on the device: android/setup/companion (C port of this script, companion_decor.py and
# bflyt_render.py). Change both together; tests/setup/test_companion.py compares them file by file.
GENERATOR_VERSION = 5
COMMON = 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs'
ASTC = dict(zip(range(0x2d, 0x3b), ((4,4),(5,4),(5,5),(6,5),(6,6),(8,5),(8,6),(8,8),(10,5),(10,6),(10,8),(10,10),(12,10),(12,12))))
LAYERS = {'G': 'Ground', 'S': 'Sky', 'U': 'UnderGround'}


def sarc(data):
    """Read named little-endian SARC entries, as in make_output_res_overlay.py."""
    if data[:4] != b'SARC' or data[6:8] != b'\xff\xfe':
        raise ValueError('Expected little-endian SARC')
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    if size != len(data):
        raise ValueError('Truncated SARC')
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attr & 0xffffff) * 4
        name = data[at:data.index(0, at, start)].decode()
        out[name] = data[start + lo:start + hi]
    return out


class RomFS:
    def __init__(self, root):
        root = Path(root)
        self.files = {f['path']: (f['offset'], f['size']) for f in json.loads((root / 'index.json').read_text())['files']}
        self.fd = os.open(root / 'romfs.bin', os.O_RDONLY)
        self.dicts = {}
        for blob in sarc(zstd.decompress(self.raw('Pack/ZsDic.pack.zs'))).values():
            dictionary = zstd.ZstdDict(blob)
            self.dicts[dictionary.dict_id] = dictionary

    def raw(self, path):
        offset, size = self.files[path]
        data = os.pread(self.fd, size, offset)
        if len(data) != size:
            raise ValueError(f'Truncated RomFS entry: {path}')
        return data

    def get(self, path):
        data = self.raw(path)
        if path.endswith('.zs'):
            did = zstd.get_frame_info(data).dictionary_id
            return zstd.decompress(data, zstd_dict=self.dicts[did] if did else None)
        return data


def byml(data):
    """The little-endian BYML v7 subset used by the sort/location tables."""
    if data[:2] != b'YB':
        raise ValueError('Expected little-endian BYML')
    kt, st, root = struct.unpack_from('<III', data, 4)
    def strings(at):
        if not at:
            return []
        n = struct.unpack_from('<I', data, at)[0] >> 8
        offsets = struct.unpack_from(f'<{n}I', data, at + 4)
        return [data[at + o:data.index(0, at + o)].decode() for o in offsets]
    keys, values = strings(kt), strings(st)
    def value(t, v):
        if t == 0xa0: return values[v]
        if t == 0xc0:
            n = struct.unpack_from('<I', data, v)[0] >> 8
            start = (v + 4 + n + 3) & ~3
            return [value(data[v + 4 + i], struct.unpack_from('<I', data, start + i * 4)[0]) for i in range(n)]
        if t == 0xc1:
            n = struct.unpack_from('<I', data, v)[0] >> 8
            return {keys[struct.unpack_from('<I', data, a)[0] & 0xffffff]: value(data[a + 3], struct.unpack_from('<I', data, a + 4)[0]) for a in range(v + 4, v + 4 + n * 8, 8)}
        if t == 0xd0: return bool(v)
        if t == 0xd1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xd2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xd3: return v
        if t == 0xd4: return struct.unpack_from('<q', data, v)[0]
        if t == 0xd5: return struct.unpack_from('<Q', data, v)[0]
        if t == 0xd6: return struct.unpack_from('<d', data, v)[0]
        if t == 0xff: return None
        raise ValueError(f'Unsupported BYML type {t:#x}')
    return value(data[root], root)


def msbt(data):
    """LBL1 bucket labels -> TXT2 strings, dropping formatting/control payloads.

    MSBT 0x0e tags carry group, type and a byte-length payload; 0x0f closing
    tags carry group/type only. Skip formatting parameters; declension tags
    (201/10) contain length-prefixed grammatical forms: keep the first, the standalone/nominative name.
    Preserve ordinary text/newlines and ruby base text.
    """
    if data[:8] != b'MsgStdBn':
        raise ValueError('Expected MSBT')
    endian = '<' if data[8:10] == b'\xff\xfe' else '>'
    if data[12] != 1:
        raise ValueError('Expected UTF-16 MSBT')
    sections = {}
    at = 0x20
    for _ in range(struct.unpack_from(endian + 'H', data, 14)[0]):
        size = struct.unpack_from(endian + 'I', data, at + 4)[0]
        sections[data[at:at + 4]] = data[at + 16:at + 16 + size]
        at = (at + 16 + size + 15) & ~15
    labels, texts = sections[b'LBL1'], sections[b'TXT2']
    count = struct.unpack_from(endian + 'I', texts)[0]
    offsets = struct.unpack_from(endian + f'{count}I', texts, 4)
    decoded = []
    for start in offsets:
        pos, pieces = start, bytearray()
        while pos + 2 <= len(texts):
            ch = struct.unpack_from(endian + 'H', texts, pos)[0]
            pos += 2
            if ch == 0: break
            if ch == 0xe:
                group, kind, length = struct.unpack_from(endian + '3H', texts, pos)
                if group == 201 and kind == 10:
                    size = struct.unpack_from(endian + 'H', texts, pos + 6)[0]
                    if length < 2 or size % 2 or size > length - 2 or pos + 6 + length > len(texts):
                        raise ValueError('Invalid MSBT declension')
                    pieces += texts[pos + 8:pos + 8 + size]
                pos += 6 + length
            elif ch == 0xf:
                pos += 4
            else:
                pieces += struct.pack(endian + 'H', ch)
        decoded.append(pieces.decode('utf-16-le' if endian == '<' else 'utf-16-be').strip())
    out = {}
    for bucket in range(struct.unpack_from(endian + 'I', labels)[0]):
        n, pos = struct.unpack_from(endian + 'II', labels, 4 + bucket * 8)
        for _ in range(n):
            size = labels[pos]
            label = labels[pos + 1:pos + 1 + size].decode()
            number = struct.unpack_from(endian + 'I', labels, pos + 1 + size)[0]
            out[label] = decoded[number]
            pos += 5 + size
    return out


def textures(data):
    if data[:4] != b'BNTX':
        raise ValueError('Expected BNTX')
    nx = data.index(b'NX  ', 0, 0x40)
    count, table = struct.unpack_from('<IQ', data, nx + 4)
    out = {}
    for i in range(count):
        at = struct.unpack_from('<Q', data, table + i * 8)[0]
        if data[at:at + 4] != b'BRTI': raise ValueError('Invalid BRTI')
        fmt, _, width, height, depth, arrays, block = struct.unpack_from('<7I', data, at + 0x1c)
        name_at = struct.unpack_from('<Q', data, at + 0x60)[0]
        name = data[name_at + 2:data.index(0, name_at + 2)].decode()
        pointers = struct.unpack_from('<Q', data, at + 0x70)[0]
        mip0 = struct.unpack_from('<Q', data, pointers)[0]
        out[name] = (fmt >> 8, width, height, block & 7, mip0, tuple(data[at + 0x58:at + 0x5c]), struct.unpack_from('<H', data, at + 0x12)[0])
    return out


def deswizzle(data, width, height, bpp, log_height):
    bh = 1 << log_height
    while bh > 1 and (height + 7) // 8 <= bh // 2:
        bh //= 2
    gobs = (width * bpp + 63) // 64
    row = width * bpp
    out = bytearray(row * height)
    for y in range(height):
        gy = y // (8 * bh) * 512 * bh * gobs + y % (8 * bh) // 8 * 512
        yp = y % 8 // 2 * 64 + y % 2 * 16
        dest = y * row
        for x in range(0, row, bpp):
            src = gy + x // 64 * 512 * bh + x % 64 // 32 * 256 + yp + x % 32 // 16 * 32 + x % 16
            block = data[src:src + bpp]
            if len(block) != bpp: raise ValueError('Truncated BNTX mip')
            out[dest:dest + bpp] = block
            dest += bpp
    return bytes(out)


def decode_texture(data, info):
    fmt, w, h, block, offset, selectors, tile_mode = info
    bw, bh, bpp = (*ASTC[fmt], 16) if fmt in ASTC else (4, 4, 8 if fmt in (0x1a, 0x1d) else 16)
    if fmt == 0x0b: bw, bh, bpp = 1, 1, 4
    if fmt == 0x02: bw, bh, bpp = 1, 1, 1
    if fmt in (0x07, 0x09): bw, bh, bpp = 1, 1, 2
    if fmt not in ASTC and fmt not in (0x02, 0x07, 0x09, 0x0b, 0x1a, 0x1c, 0x1d, 0x1e, 0x20):
        raise ValueError(f'Unsupported BNTX format {fmt:#x}')
    wb, hb = (w + bw - 1) // bw, (h + bh - 1) // bh
    linear = data[offset:offset + wb * hb * bpp] if tile_mode == 1 else deswizzle(memoryview(data)[offset:], wb, hb, bpp, block)
    if fmt in ASTC:
        pixels = decoder.decode_astc(linear, w, h, bw, bh)
    elif fmt == 0x02:
        red = Image.frombytes('L', (w, h), linear)
        image = Image.merge('RGBA', (red, Image.new('L', (w,h)), Image.new('L', (w,h)), Image.new('L', (w,h), 255)))
        pixels = None
    elif fmt == 0x07:
        image = Image.frombytes('RGB', (w, h), linear, 'raw', 'BGR;16').convert('RGBA')
        pixels = None
    elif fmt == 0x09:
        image = Image.merge('RGBA', (Image.frombytes('L', (w,h), linear[::2]),
                                   Image.frombytes('L', (w,h), linear[1::2]),
                                   Image.new('L', (w,h)), Image.new('L', (w,h), 255)))
        pixels = None
    elif fmt == 0x0b:
        image = Image.frombytes('RGBA', (w, h), linear)
        pixels = None
    else:
        method = {0x1a: decoder.decode_bc1, 0x1c: decoder.decode_bc3, 0x1d: decoder.decode_bc4, 0x1e: decoder.decode_bc5, 0x20: decoder.decode_bc7}[fmt]
        pixels = method(linear, w, h)
    if pixels is not None:
        image = Image.frombytes('RGBA', (w, h), pixels, 'raw', 'BGRA')
    channels = (Image.new('L', (w, h), 0), Image.new('L', (w, h), 255), *image.split())
    if any(s > 5 for s in selectors): raise ValueError(f'Invalid BNTX channel selectors {selectors}')
    return Image.merge('RGBA', [channels[s] for s in selectors])


def save_webp(image, path, quality=85):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, 'WEBP', quality=quality, method=3, exact=True)


_READER = None
_OUT = None

def worker_init(root, out):
    global _READER, _OUT
    _READER, _OUT = RomFS(root), Path(out)


def texture_job(job):
    source, destination, limit, quality = job
    data = _READER.get(source)
    infos = textures(data)
    if len(infos) != 1: raise ValueError(f'Expected one texture: {source}')
    image = decode_texture(data, next(iter(infos.values())))
    if limit:
        image.thumbnail((limit, limit), Image.Resampling.LANCZOS)
    save_webp(image, _OUT / destination, quality)
    return destination


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')


# Texture names are the shipped Common.Product.140 names, visually checked on
# contact sheets. BC4 channel selectors (ONE, ONE, ONE, RED) produce tintable
# white-on-alpha masks; ASTC artwork keeps its authored colors.
UI_TEXTURES = {
    'heart': 'HeartW_00^s', 'heart_empty': 'HeartFrame_00^s',
    'heart_yellow': 'HeartW_00^s', 'heart_broken': 'HeartBreakW_00^s',
    'rupee': 'Nt_RupeeIconNew_00^w', 'poe': 'NumDisplay_82x82_MinusRupee^w',
    'battery': 'EnergyGaugeBigBaseFrame_00^s', 'battery_fill': 'EnergyGaugeCore_00^s',
    'stamina_ring': 'BonusIcon72_Ganbari_00^s',
    'cat_weapons': 'IconWeapon_72x72^s', 'cat_bows': 'IconBow_72x72^s',
    'cat_shields': 'IconShield_72x72^s', 'cat_armor': 'IconEquipment_72x72^s',
    'cat_materials': 'IconMaterial_72x72^s', 'cat_food': 'IconCuisine_72x72^s',
    # PaPouchTab_00_Pattern frames 6 and 8: capsule silhouette / key-item star.
    'cat_zonai': 'IconRecycle_72x72^s', 'cat_keyItems': 'Nt_PauseIconL_06^s',
    'map_player': 'Nt_MapPlayerIconNew_00^s',
    'marker_shrine': 'MapIconDungeon_L_Icon^s', 'marker_lightroot': 'LoadArrivalPoint_00^w',
    'marker_tower': 'MapIconTower_00^s',
    'marker_stable': 'MapIconBuildingStableInn_58x58^w',
    'marker_town': 'MapIconBuildingVillage_58x58^w',
    'background': 'PlayerStatusBG_00^t', 'frame_corner': 'Nt_PauseCorner_00^s',
}
for _id, _texture in {'Wind':'Wind', 'Fire':'Fire', 'Water':'Water', 'Electric':'Thunder', 'Soul':'Soul'}.items():
    UI_TEXTURES['sage_' + _id] = f'SageSoulIcon_{_texture}^s'
for _id, _texture in {'Sword':'Weapon','Pot':'Cuisine','Human':'Human','Rhombus':'Square','Heart':'Heart','Star':'Star','TreasureBox':'TreasureBox','Skull':'Skull','Leaf':'Leaf','Ore':'Ore'}.items():
    UI_TEXTURES['stamp_' + _id] = f'MapIcon{_texture}_72x72^w'
for _n, _id in enumerate(('Red','Blue','Yellow','Green','Purple','LightBlue')):
    UI_TEXTURES['pin_' + _id] = f'MapPinNewFLSmall{_n}_00^' + ('s' if _n == 5 else 'w')
EFFECT_TEXTURES = {
    'AttackUp':'AttackCook', 'DefenseUp':'ShieldUp', 'AllSpeed':'GoGo',
    'ResistCold':'Cool', 'ResistHot':'Hot', 'ResistBurn':'Fire',
    'ResistElectric':'Elect', 'QuietnessUp':'Noise', 'SwimSpeedUp':'Swim',
    'ClimbSpeedUp':'Mountain', 'SandMoveUp':'Sabaku', 'SnowMoveUp':'Sabaku',
    'LifeMaxUp':'Heart', 'StaminaRecover':'Ganbari', 'ExStaminaMaxUp':'GanbariEx',
    'LifeRepair':'HeartRepair', 'MiasmaGuard':'MiasmaGuard', 'NotSlippy':'Slip',
    'LightEmission':'Luminous', 'RupeeGuard':'Rupee', 'DivingMobility':'Diving',
    'DecreaseZonauEnergy':'DecreaseZonauEnergy', 'AttackUpCold':'AttackCold',
    'AttackUpHot':'AttackHot', 'AttackUpThunderstorm':'AttackThun',
}
ABILITY_ACTORS = {
    'UltraHand':'Obj_UltraHand', 'OneTouchBond':'Obj_OneTouchBond',
    'CeilingClipper':'Obj_Tooreroof', 'ReverseRecorder':'Obj_ReverseRecorder',
    'AutoBuilder':'Obj_AutoBuilder',
}
SAGE_ACTORS = {'Wind':'Rito','Fire':'Goron','Water':'Zora','Electric':'Gerudo','Soul':'Zonau'}


def rsdb(reader, table):
    """RSDB/<table>.Product.<version>.rstbl.byml.zs: the rows (a list of dicts keyed by __RowId)."""
    paths = [p for p in sorted(reader.files) if p.startswith(f'RSDB/{table}.Product.') and p.endswith('.rstbl.byml.zs')]
    if len(paths) != 1: raise ValueError(f'Expected one RSDB {table} table, found {len(paths)}')
    return byml(reader.get(paths[0]))


def base_actors(reader, actor):
    """The actors <actor> is built from: its ActorParam $parent chain, then its GameParameterTable and that table's
    $parent chain (stems of the Work/... paths, in that order, without <actor>)."""
    path = f'Pack/Actor/{actor}.pack.zs'
    if path not in reader.files: return []
    pack, out = sarc(reader.get(path)), []
    def stem(work): return work.rsplit('/', 1)[-1].split('.', 1)[0]
    def follow(member, pattern):
        while member in pack:
            parent = byml(pack[member]).get('$parent')
            if not parent: break
            out.append(stem(parent))
            member = pattern.format(stem(parent))
    param = f'Actor/{actor}.engine__actor__ActorParam.bgyml'
    if param not in pack: return []
    follow(param, 'Actor/{}.engine__actor__ActorParam.bgyml')
    table = (byml(pack[param]).get('Components') or {}).get('GameParameterTableRef')
    if table:
        table = table.lstrip('?')
        out.append(stem(table))
        follow(table, 'GameParameter/GameParameterTable/{}.engine__actor__GameParameterTable.bgyml')
    seen = {actor}
    return [a for a in out if not (a in seen or seen.add(a))]


def pouch_name_sources(reader, named, common):
    """Pouch/fuse actors -> their display actor, or @<AttachmentCommonName> message label.

    Pouch names use ActorInfo ActorName and the named actor's $parent chains. Fuse-only objects additionally use
    ActorMsg/Attachment and AttachmentActorInfo.AttachmentCommonName -> StaticMsg/AttachmentCommonName. These are
    the game's authored material names, not the adjectives used to construct a combined weapon's title.
    """
    info = {row['__RowId']: row.get('ActorName') for row in rsdb(reader, 'ActorInfo')}
    attachments = rsdb(reader, 'AttachmentActorInfo')
    out = {row['__RowId']: '@' + row['AttachmentCommonName'] for row in attachments
           if row['__RowId'] not in named and row.get('AttachmentCommonName', '') + '_Name' in common}
    named = named | out.keys()
    for row in rsdb(reader, 'PouchActorInfo') + attachments:
        actor = row['__RowId']
        if actor in named or actor in out: continue
        alias = info.get(actor)
        target = alias if alias and alias != actor and alias in named else next((a for a in base_actors(reader, actor) if a in named), None)
        if target: out[actor] = target
    return out


def extract_text(reader, out):
    languages, actors, sources = [], set(), None
    for path in sorted(reader.files):
        if not path.startswith('Mals/') or not path.endswith('.sarc.zs'): continue
        lang = Path(path).name.split('.')[0]
        archive = sarc(reader.get(path))
        pouch = msbt(archive['ActorMsg/PouchContent.msbt'])
        attachment = msbt(archive['ActorMsg/Attachment.msbt'])
        common = msbt(archive['StaticMsg/AttachmentCommonName.msbt'])
        items = {}
        for label, text in pouch.items():
            for suffix, key in (('_Name', 'name'), ('_Caption', 'desc')):
                if label.endswith(suffix):
                    actor = label[:-len(suffix)]
                    items.setdefault(actor, {})[key] = text
                    actors.add(actor)
                    break
        for label, text in attachment.items():
            if label.endswith('_Name'):
                actor = label[:-5]
                items.setdefault(actor, {}).setdefault('name', text)
                actors.add(actor)
        if sources is None:
            sources = pouch_name_sources(reader, {actor for actor, item in items.items() if 'name' in item}, common)
        for actor, target in sources.items():
            source = {'name': common[target[1:] + '_Name']} if target.startswith('@') else items.get(target, {})
            if actor not in items and 'name' in source:
                items[actor] = dict(source)
                actors.add(actor)
        layout = {name: msbt(data) for name, data in archive.items() if name.startswith(('LayoutMsg/', 'StaticMsg/')) and name.endswith('.msbt')}
        categories = layout['LayoutMsg/Pouch_00.msbt']
        ui = {key: categories[f'Pa_SlideList_00-Pa_Category_{i:02d}-T_Text_00'] for i, key in enumerate(('armor','bows','shields','weapons','materials','food','zonai','keyItems'))}
        # Keep all authored labels (namespaced to avoid collisions) available
        # for companion screens beyond the canonical category/ability keys.
        for name, labels in layout.items():
            ui.update({name[:-5] + '/' + key: text for key, text in labels.items() if text})
        abilities = {key: items[actor]['name'] for key, actor in ABILITY_ACTORS.items() if actor in items}
        sages = {key: items['Obj_SageSoul_' + actor]['name'] for key, actor in SAGE_ACTORS.items() if 'Obj_SageSoul_' + actor in items}
        places = msbt(archive['LocationMsg/Location.msbt'])
        write_json(out / 'text' / (lang + '.json'), dict(items=items, ui=ui, effects=layout.get('StaticMsg/Buff.msbt', {}), abilities=abilities, sages=sages, places=places))
        languages.append(lang)
    if not languages or not actors: raise ValueError('No Mals pouch text found')
    return languages, actors, sources


def icon_jobs(reader, actors, sources):
    # PouchContent is the authoritative actor set. Sort-table dye colors are
    # aliases, not 15 extra copies of each armor image. Include recipe effect
    # variants and non-capsule Zonai parts as well as their pouch capsules.
    sort = byml(reader.get('UI/PouchSortTable/PouchSortTable.game__ui__PouchSortTable.bgyml'))
    colors = set(sort['ArmorDyeColor']) - {'None'}
    jobs, aliases, names = [], {}, set()
    for path in sorted(reader.files):
        if not path.startswith(('UI/Tex/Icon/', 'UI/Tex/SpecialParts/')) or not path.endswith('.bntx.zs'): continue
        actor = Path(path).name[:-8]
        if actor in names: continue
        if actor.startswith('Armor_') and actor.rsplit('_', 1)[-1] in colors:
            base = actor.rsplit('_', 1)[0]
            if base in actors: aliases[actor] = base
            continue
        if actor not in actors and not actor.startswith(('Item_Cook_', 'SpObj_')): continue
        names.add(actor)
        jobs.append((path, f'icons/{actor}.webp', 128, 85))
    # Elixirs have no unbuffed texture. Keep every actual variant and select
    # the stamina-recovery bottle as the representative for the actor-only UI.
    if 'Item_Cook_C_17_StaminaRecover' in names:
        aliases['Item_Cook_C_17'] = 'Item_Cook_C_17_StaminaRecover'
    if 'Obj_Battery_Get_Capacity03' not in names:
        path = 'UI/Tex/Icon/Obj_Battery_Get_Capacity03.bntx.zs'
        if path in reader.files:
            names.add('Obj_Battery_Get_Capacity03')
            jobs.append((path, 'icons/Obj_Battery_Get_Capacity03.webp', 128, 85))
            aliases['Obj_Battery_Get'] = 'Obj_Battery_Get_Capacity03'
    # Pouch actors that show another actor's text and have no icon of their own show that actor's icon.
    for actor, target in sources.items():
        if actor not in names: aliases[actor] = target
    return jobs, sorted(names), {k:v for k,v in aliases.items() if v in names}


def extract_ui(reader, out):
    data = sarc(reader.get(COMMON))['timg/__Combined.bntx']
    infos = textures(data)
    mapping = dict(UI_TEXTURES)
    for key, stem in EFFECT_TEXTURES.items():
        choices = [n for n in infos if n.startswith(f'BonusIcon72_{stem}_00^')]
        if choices: mapping['effect_' + key] = choices[0]
    ui, sources = {}, {}
    for key, name in sorted(mapping.items()):
        if name not in infos: continue
        image = decode_texture(data, infos[name])
        if key in ('marker_stable', 'marker_town'):
            # These two ASTC sprites are black ink on white badges, unlike
            # the tintable map masks. Export the actual ink silhouette, not a
            # solid rectangle when Canvas applies its common marker tint.
            alpha = ImageChops.multiply(ImageChops.invert(image.convert('L')), image.getchannel('A'))
            white = Image.new('L', image.size, 255)
            image = Image.merge('RGBA', (white, white, white, alpha))
        destination = 'ui/' + key + '.webp'
        save_webp(image, out / destination)
        ui[key], sources[key] = destination, name
    for key, actor in ABILITY_ACTORS.items():
        path = reader.get(f'UI/Tex/Icon/{actor}.bntx.zs')
        image = decode_texture(path, next(iter(textures(path).values())))
        image.thumbnail((128, 128), Image.Resampling.LANCZOS)
        destination = 'ui/ability_' + key + '.webp'
        save_webp(image, out / destination)
        ui['ability_' + key], sources['ability_' + key] = destination, f'UI/Tex/Icon/{actor}.bntx.zs'
    return ui, sources


def map_jobs(reader):
    pattern = re.compile(r'UI/Map/MainField/([GSU])_(\d\d)-(\d\d)_(01|02)(?:_([TF]+))?(?:_([AB](?:_[AB])*))?\.bntx\.zs$')
    selected = {}
    for path in sorted(reader.files):
        m = pattern.fullmatch(path)
        if not m: continue
        layer, col, row, lod, variant, event = m.groups()
        key = (layer, int(col), int(row), int(lod))
        rank = ((variant or '').count('T'), len(event or ''), event or '', variant or '')
        if key not in selected or rank > selected[key][0]:
            selected[key] = (rank, path)
    expected = {(l,c,r,d) for l in LAYERS for c in range(12) for r in range(10) for d in (1,2)}
    if set(selected) != expected:
        raise ValueError(f'Incomplete MainField map: missing {sorted(expected - set(selected))[:10]}')
    jobs = [(path, f'map/{LAYERS[layer]}/{lod}/{col}_{row}.webp', 0, 80) for (layer,col,row,lod), (_,path) in sorted(selected.items())]
    return jobs


def map_overviews(out):
    # Resize each tile before stitching: only ~20 MB for a full overview.
    for layer in LAYERS.values():
        image = Image.new('RGB', (2400, 2000))
        for col in range(12):
            for row in range(10):
                with Image.open(out / f'map/{layer}/2/{col}_{row}.webp') as tile:
                    image.paste(tile.resize((200,200), Image.Resampling.LANCZOS), (col * 200, row * 200))
        save_webp(image, out / f'map/{layer}/overview.webp', 80)


def extract_markers(reader, out):
    markers = []
    # These location-area tables contain authored map display positions, not
    # guessed centroids of placement volumes. Text labels remain language-neutral.
    # Match actual Sky LocationMarker instance IDs. Altitude is not a layer:
    # the ground shrine on Death Mountain is above several sky shrines.
    sky_ids = set()
    for path in reader.files:
        if not path.startswith('Banc/MainField/Sky/') or not path.endswith('.byml.zs'): continue
        blob = reader.get(path)
        if b'Dungeon' not in blob: continue
        for actor in byml(blob).get('Actors', []):
            if actor.get('Gyaml') == 'LocationMarker':
                sky_ids.add(str(actor['Hash']))
    for field, fixed_layer in (('MainField', None), ('MinusField', 'UnderGround')):
        path = f'Banc/{field}/LocationArea/{field}.locationarea.byml.zs'
        if path not in reader.files: continue
        table = byml(reader.get(path))
        for group, kind in (('Shrine','shrine'), ('CheckPoint','lightroot'), ('Tower','tower'), ('Stable','stable'), ('City','town')):
            for entry in table.get(group, []):
                label = entry['LocationName'].split('/')[-1].split('.')[0]
                for i, pos in enumerate(entry.get('Trans', [])):
                    if len(pos) != 3: continue
                    instance_ids = entry.get('InstanceID', [])
                    instance = instance_ids[i] if i < len(instance_ids) else None
                    layer = fixed_layer or ('Sky' if instance in sky_ids else 'Ground')
                    markers.append(dict(id=f'{field}/{label}/{i}', type=kind, label=label, x=pos[0], y=pos[1], z=pos[2], layer=layer))
    if markers: write_json(out / 'map/markers.json', markers)
    return bool(markers)


def extract_font(reader, out):
    archive = sarc(reader.get('Font/Font.Nin_NX_NVN.bfarc.zs'))
    blob = archive['scft/ninP_RodinNTLG-M_DH_003_subset.bfotf']
    key = struct.unpack_from('<I', blob)[0] ^ 0x18029a7f
    size = int.from_bytes(struct.pack('<I', struct.unpack_from('<I', blob, 4)[0] ^ key), 'big')
    body, font = blob[8:8 + size], bytearray()
    for i in range(0, size - size % 4, 4):
        font += struct.pack('<I', struct.unpack_from('<I', body, i)[0] ^ key)
    font += body[size - size % 4:]
    if font[:4] not in (b'OTTO', b'\0\1\0\0'): raise ValueError('Invalid decoded game font')
    path = 'fonts/main.otf' if font[:4] == b'OTTO' else 'fonts/main.ttf'
    target = out / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(font)
    return {'main': path}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1), help='texture worker count (default: up to 8)')
    parser.add_argument('--review-dir', type=Path, help='optional offline menu renders and named decor contact sheet (never put in source control)')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32: parser.error('--jobs must be 1..32')
    start = time.monotonic()
    reader = RomFS(args.romfs)
    out = args.out / 'companion'
    # Generate into a sibling staging tree. A failed run never leaves a valid
    # index pointing at partially updated assets, nor stale output files.
    staging = args.out / f'.companion-{os.getpid()}'
    staging.mkdir(parents=True, exist_ok=False)
    try:
        languages, actors, name_sources = extract_text(reader, staging)
        jobs, icons, aliases = icon_jobs(reader, actors, name_sources)
        jobs += map_jobs(reader)
        ui, sources = extract_ui(reader, staging)
        fonts = extract_font(reader, staging)
        markers = extract_markers(reader, staging)
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs, initializer=worker_init, initargs=(args.romfs, staging)) as pool:
            results = pool.map(texture_job, jobs, chunksize=8)
            archive = sarc(reader.get(COMMON))
            combined = archive['timg/__Combined.bntx']
            infos = textures(combined)
            decor, palette, page_icons, decor_sources = extract_decor(
                archive, staging, lambda name: decode_texture(combined, infos[name]) if name in infos else None,
                save_webp, args.review_dir)
            for _ in results: pass
        map_overviews(staging)
        index = dict(schema=1, generator=GENERATOR_VERSION, gameVersion='1.4.3', languages=languages,
                     defaultLanguage='USen', icons=icons, iconAliases=aliases, ui=ui, uiSources=sources,
                     decor=decor, palette=palette, pageIcons=page_icons, decorSources=decor_sources,
                     map=dict(world=dict(minX=-6000,maxX=6000,minZ=-5000,maxZ=5000), cols=12,rows=10,
                              layers=list(LAYERS.values()), lods=[dict(level=1,tilePx=840),dict(level=2,tilePx=300)],
                              variant='All-T revealed terrain; longest A/B suffix chain selects complete composites; prefer B over A [INFERENCE: post-event]',
                              orientation='Native column/row order; tile 0,0 = minX,minZ; +Z down; static alignment checked at Lookout Landing, Kakariko, Hateno and Tarrey Town'),
                     fonts=fonts)
        if markers: index['markers'] = 'map/markers.json'
        write_json(staging / 'index.json', index)
        sizes = {name:sum(p.stat().st_size for p in (staging / name).rglob('*') if p.is_file()) for name in ('icons','ui','decor','map','text','fonts')}
        if sum(sizes.values()) > 120 * 1024 * 1024: raise ValueError('Companion assets exceed the 120 MiB size limit')
        if out.exists(): shutil.rmtree(out)
        staging.rename(out)
        print(json.dumps(dict(output=str(out), seconds=round(time.monotonic()-start,2), icons=len(icons), languages=len(languages),
                              MiB={key:round(value / 1024**2,2) for key,value in sizes.items()})))
    finally:
        os.close(reader.fd)
        if staging.exists(): shutil.rmtree(staging)


if __name__ == '__main__':
    main()

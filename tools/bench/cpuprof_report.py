#!/usr/bin/env python3
"""Report a native cpu-clock recording using the NDK's ReportLib (no report-text parsing).

Usage: cpuprof_report.py DATA --symbols DIR [--ndk-simpleperf DIR] [--out FILE] [--top N]
                         [--capture-manifest FILE]
The default output is DATA with its suffix replaced by .json; stdout is a readable summary.
DIR must contain exactly one unstripped libtotk.so, directly or in a symbol-cache tree.
The recorded GNU build ID must match that ELF. Legacy builds without GNU IDs require a
capture manifest binding this recording and exact symbols to the captured APK's allocated
ELF sections. A private symfs index makes a flat symbols folder usable without modifying
it. NDK imports/native loading happen only when reporting.

Each sample contributes its period, not one vote. cpu-clock periods are nanoseconds of
sampled CPU time, not elapsed frame time. Self charges the leaf; inclusive charges the
leaf and each distinct (DSO, symbol) in its available callers once, including recursion.
Ranking percentages use that thread's total period, before --top truncation. Inclusive
rows overlap and are NOT a wall-time critical path or evidence of waiting/GPU cost.
Missing symbols are retained as [unknown]. Caller presence measures callchain coverage,
not unwind completeness; incomplete or missing chains understate inclusive attribution.
"""
import argparse
from collections import Counter
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile

SCHEMA = 'totk-cpuprof/1'
NDK_VERSION = '28.2.13676358'
GROUPS = ('walker', 'recorders', 'submit', 'game', 'other')
LIMITATIONS = [
    'Sample periods weight sampled on-CPU work; CPU milliseconds are cpu-clock nanoseconds / 1e6, not wall time.',
    'Inclusive rows overlap, deduplicate recursive (DSO, symbol) frames per sample, and are not the wall-time critical path.',
    'Missing symbols remain [unknown], coalescing unresolved addresses within each DSO; only libtotk.so build identity is explicitly verified by this report.',
    'Caller presence is reported by samples and period, but does not prove a complete unwind; missing/truncated callchains understate inclusive cost.',
    'Thread groups are inferred from sampled comm names; a renamed thread uses its latest sampled name.',
]


class ReportError(ValueError):
    """An unusable recording or symbol input."""


def read_elf_identity(path):
    """Return {build_id: raw lowercase hex or None, has_symtab: bool, has_debug_info: bool}.

    Read GNU notes and section metadata with stdlib only, including stripped ELF files
    whose notes survive only in PT_NOTE. No target code or host binutils are executed.
    """
    return _inspect_elf(path)


def elf_alloc_sha256(path):
    """Hash SHF_ALLOC section names/type/flags/address/size and bytes, independent of debug stripping.

    SHT_NOBITS contributes metadata only. File offsets, section indexes and non-allocated
    debug/symbol tables are deliberately excluded. This is a capture provenance identity,
    not a replacement for a present but mismatching GNU build ID.
    """
    return _inspect_elf(path, include_alloc_hash=True)['elf_alloc_sha256']


def _inspect_elf(path, include_alloc_hash=False):
    path = Path(path)
    with path.open('rb') as elf:
        file_size = path.stat().st_size

        def read_at(offset, size):
            if offset < 0 or size < 0 or offset + size > file_size:
                raise ReportError(f'{path}: truncated or invalid ELF range')
            elf.seek(offset)
            data = elf.read(size)
            if len(data) != size:
                raise ReportError(f'{path}: truncated ELF')
            return data

        ident = read_at(0, 16)
        if ident[:4] != b'\x7fELF' or ident[4] not in (1, 2) or ident[5] not in (1, 2):
            raise ReportError(f'{path}: expected an ELF32/ELF64 file')
        endian = '<' if ident[5] == 1 else '>'
        is_64 = ident[4] == 2

        def unpack_at(fmt, offset):
            return struct.unpack(endian + fmt, read_at(offset, struct.calcsize(endian + fmt)))

        header = unpack_at('HHIQQQIHHHHHH' if is_64 else 'HHIIIIIHHHHHH', 16)
        phoff, shoff = header[4:6]
        phsize, phnum, shsize, shnum, strindex = header[8:13]
        section_fmt = 'IIQQQQIIQQ' if is_64 else 'IIIIIIIIII'
        section_size = struct.calcsize(endian + section_fmt)
        sections = []
        if shoff:
            if shsize < section_size:
                raise ReportError(f'{path}: invalid ELF section header size')
            first = unpack_at(section_fmt, shoff)
            shnum = shnum or first[5]
            if strindex == 0xffff:
                strindex = first[6]
            if phnum == 0xffff:
                phnum = first[7]
            read_at(shoff, shsize * shnum)
            sections = [unpack_at(section_fmt, shoff + i * shsize) for i in range(shnum)]
        elif shnum or strindex or phnum == 0xffff:
            raise ReportError(f'{path}: ELF section table is missing')

        names = b''
        if strindex:
            if strindex >= len(sections):
                raise ReportError(f'{path}: invalid ELF section name table')
            strings = sections[strindex]
            names = read_at(strings[4], strings[5])
        def section_name(section):
            offset = section[0]
            if offset >= len(names):
                raise ReportError(f'{path}: invalid ELF section name offset')
            return names[offset:].split(b'\0', 1)[0]

        section_names = {section_name(s) for s in sections} if names else set()
        alloc_hash = None
        if include_alloc_hash:
            allocated = [s for s in sections if s[2] & 2]  # SHF_ALLOC
            if not allocated or not names:
                raise ReportError(f'{path}: missing named SHF_ALLOC sections; cannot establish allocated ELF identity')
            digest = hashlib.sha256(b'totk-elf-alloc-sha256-v1\0')
            digest.update(struct.pack('<BBHHI', ident[4], ident[5], header[0], header[1], header[6]))
            for section in sorted(allocated, key=lambda s: (s[3], section_name(s))):
                name = section_name(section)
                digest.update(struct.pack('<I', len(name)) + name)
                digest.update(struct.pack('<IQQQ', section[1], section[2], section[3], section[5]))
                if section[1] != 8:  # SHT_NOBITS has no file bytes.
                    offset, remaining = section[4:6]
                    while remaining:
                        count = min(remaining, 1024 * 1024)
                        digest.update(read_at(offset, count))
                        offset += count
                        remaining -= count
            alloc_hash = digest.hexdigest()
        note_ranges = {(s[4], s[5]) for s in sections if s[1] == 7}  # SHT_NOTE
        program_fmt = 'IIQQQQQQ' if is_64 else 'IIIIIIII'
        if phnum:
            if phsize < struct.calcsize(endian + program_fmt):
                raise ReportError(f'{path}: invalid ELF program header size')
            read_at(phoff, phsize * phnum)
            for i in range(phnum):
                ph = unpack_at(program_fmt, phoff + i * phsize)
                if ph[0] == 4:  # PT_NOTE
                    note_ranges.add((ph[2], ph[5]) if is_64 else (ph[1], ph[4]))
        build_ids = set()
        for offset, size in note_ranges:
            notes = read_at(offset, size)
            pos = 0
            while pos + 12 <= len(notes):
                namesz, descsz, kind = struct.unpack_from(endian + 'III', notes, pos)
                pos += 12
                descpos = pos + (namesz + 3) // 4 * 4
                end = descpos + (descsz + 3) // 4 * 4
                if end > len(notes):
                    raise ReportError(f'{path}: truncated ELF note')
                if kind == 3 and notes[pos:pos + namesz].rstrip(b'\0') == b'GNU' and descsz:
                    build_ids.add(notes[descpos:descpos + descsz].hex())
                pos = end
        if len(build_ids) > 1:
            raise ReportError(f'{path}: conflicting GNU build IDs')
        result = {
            'build_id': next(iter(build_ids), None),
            'has_symtab': any(s[1] == 2 and s[5] > 0 for s in sections),
            'has_debug_info': bool(section_names & {b'.debug_info', b'.zdebug_info'}),
        }
        if include_alloc_hash:
            result['elf_alloc_sha256'] = alloc_hash
        return result


def simpleperf_build_id(build_id):
    """NDK ReadElf.pad_build_id(): 20 bytes, zero-padded on the right, with 0x.

    Do not truncate long GNU IDs: that would no longer prove an exact match.
    """
    value = (build_id or '').lower().removeprefix('0x')
    if not value or len(value) % 2 or not re.fullmatch('[0-9a-f]+', value) or not int(value, 16):
        raise ReportError('missing or invalid GNU build ID')
    if len(value) > 40:
        raise ReportError('GNU build ID exceeds ReportLib\'s 20-byte identity; cannot prove an exact match')
    return '0x' + value.ljust(40, '0')


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def validate_capture_manifest(path, data, symbol_file):
    """Check recording and symbol bytes before permitting absent GNU IDs.

    The capture helper attests the installed APK/commit and compares its allocated ELF
    image with symbols. This offline report cannot independently re-observe that device.
    """
    try:
        manifest = json.loads(Path(path).read_text(encoding='utf-8'))
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise ReportError(f'{path}: invalid capture manifest JSON') from exc
    if not isinstance(manifest, dict):
        raise ReportError(f'{path}: capture manifest must be an object')
    for key in ('data_sha256', 'symbols_sha256', 'elf_alloc_sha256', 'apk_sha256'):
        value = manifest.get(key)
        if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
            raise ReportError(f'{path}: capture manifest requires {key} as lowercase SHA-256 hex')
    if not isinstance(manifest.get('commit'), str) or not re.fullmatch('[0-9a-f]{12}', manifest['commit']):
        raise ReportError(f'{path}: capture manifest requires the installed 12-hex commit')
    if file_sha256(data) != manifest['data_sha256']:
        raise ReportError(f'{data}: recording SHA-256 mismatch with capture manifest')
    if file_sha256(symbol_file) != manifest['symbols_sha256']:
        raise ReportError(f'{symbol_file}: symbols SHA-256 mismatch with capture manifest')
    if elf_alloc_sha256(symbol_file) != manifest['elf_alloc_sha256']:
        raise ReportError(f'{symbol_file}: allocated ELF SHA-256 mismatch with capture manifest')
    return {key: manifest[key] for key in
            ('data_sha256', 'symbols_sha256', 'elf_alloc_sha256', 'apk_sha256', 'commit')}


def find_symbols(directory, allow_missing_id=False):
    directory = Path(directory).resolve()
    if not directory.is_dir():
        raise ReportError(f'symbols directory does not exist: {directory}')
    candidates = sorted({p.resolve() for p in directory.rglob('libtotk.so') if p.is_file()})
    if len(candidates) != 1:
        raise ReportError(f'{directory}: expected exactly one unstripped libtotk.so, found {len(candidates)}')
    path = candidates[0]
    identity = read_elf_identity(path)
    if not identity['build_id']:
        if not allow_missing_id:
            raise ReportError(f'{path}: missing GNU build ID; supply --capture-manifest to verify exact symbols')
    else:
        simpleperf_build_id(identity['build_id'])
    if not identity['has_symtab']:
        raise ReportError(f'{path}: missing .symtab; supply the unstripped libtotk.so from the captured build')
    return path, identity


def default_simpleperf_dir():
    ndk = os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
    if ndk:
        return Path(ndk) / 'simpleperf'
    sdk = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    return (Path(sdk) if sdk else Path.home() / 'Android/Sdk') / 'ndk' / NDK_VERSION / 'simpleperf'


def load_report_lib(directory):
    """Load only the explicitly selected NDK wrapper; ordinary imports/tests need no NDK."""
    directory = Path(directory).resolve()
    wrapper = directory / 'simpleperf_report_lib.py'
    if not wrapper.is_file():
        raise ReportError(f'{wrapper} not found; use --ndk-simpleperf DIR (NDK {NDK_VERSION}/simpleperf)')
    sys.path.insert(0, str(directory))
    try:
        module = importlib.import_module('simpleperf_report_lib')
        module_path = module.__file__
        if module_path is None or Path(module_path).resolve() != wrapper:
            raise ReportError('a different NDK ReportLib is already imported; run this report in a fresh Python process')
        return module.ReportLib
    except ImportError as exc:
        raise ReportError(f'cannot import NDK ReportLib from {directory}: {exc}') from exc
    finally:
        sys.path.pop(0)


def thread_group(name):
    if name == 'totk-walk':
        return 'walker'
    if name.startswith('totk-recw'):
        return 'recorders'
    if name == 'totk-submit':
        return 'submit'
    if name.startswith('g-'):
        return 'game'
    return 'other'


def symbol_key(symbol):
    dso = symbol.dso_name or '[unknown]'
    name = symbol.symbol_name or '[unknown]'
    if name in ('unknown', '[unknown]') or re.fullmatch(r'0x[0-9a-fA-F]+', name):
        name = '[unknown]'
    return dso, name


def percent(value, total):
    return value * 100.0 / total if total else 0.0


def rankings(periods, counts, total, top):
    return [{'dso': dso, 'symbol': name, 'period': period, 'sample_count': counts[dso, name],
             'cpu_ms': period / 1e6, 'percent': percent(period, total)}
            for (dso, name), period in sorted(periods.items(), key=lambda x: (-x[1], x[0]))[:top]]


def aggregate_report(lib, identity, top=20, allow_missing_id=False):
    """Consume native sample/leaf/callchain objects before ReportLib advances their storage."""
    if top < 1:
        raise ReportError('--top must be positive')
    expected = simpleperf_build_id(identity['build_id']) if identity['build_id'] else None
    if expected is None and not allow_missing_id:
        raise ReportError('missing supplied GNU build ID; cannot verify exact symbols without a capture manifest')
    recorded_dsos = {}
    threads = {}
    event_name = None

    def frame(symbol):
        dso, name = symbol_key(symbol)
        if dso.rsplit('/', 1)[-1] == 'libtotk.so' and dso not in recorded_dsos:
            try:
                recorded = lib.GetBuildIdForPath(dso)
            except AssertionError:
                recorded = ''
            raw = (recorded or '').lower().removeprefix('0x')
            absent = not raw or bool(re.fullmatch('0+', raw))
            try:
                actual = None if absent else simpleperf_build_id(recorded)
            except ReportError as exc:
                raise ReportError(f'{dso}: missing or unusable recorded libtotk.so build ID; cannot verify exact symbols') from exc
            if actual is not None and actual != expected:
                raise ReportError(f'{dso}: libtotk.so build ID mismatch: recorded {actual}, supplied {expected}')
            if actual is None and not allow_missing_id:
                raise ReportError(f'{dso}: missing recorded libtotk.so build ID; cannot verify exact symbols without a capture manifest')
            recorded_dsos[dso] = actual
        return dso, name

    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        event = lib.GetEventOfCurrentSample().name
        if event.split(':', 1)[0] != 'cpu-clock':
            raise ReportError(f'expected cpu-clock nanosecond periods, found {event!r}; record with -e cpu-clock:u')
        if event_name is not None and event != event_name:
            raise ReportError(f'expected a single cpu-clock event, found {event_name!r} and {event!r}')
        event_name = event
        leaf = frame(lib.GetSymbolOfCurrentSample())
        chain = lib.GetCallChainOfCurrentSample()
        inclusive = {leaf}
        for i in range(chain.nr):
            inclusive.add(frame(chain.entries[i].symbol))
        key = (sample.pid, sample.tid)
        if key not in threads:
            threads[key] = {'pid': sample.pid, 'tid': sample.tid, 'names': set(), 'period': 0, 'sample_count': 0,
                            'self_periods': Counter(), 'self_counts': Counter(),
                            'inclusive_periods': Counter(), 'inclusive_counts': Counter(),
                            'coverage': Counter()}
        thread = threads[key]
        thread['name'] = sample.thread_comm
        thread['names'].add(sample.thread_comm)
        thread['period'] += sample.period
        thread['sample_count'] += 1
        thread['self_periods'][leaf] += sample.period
        thread['self_counts'][leaf] += 1
        for key in inclusive:
            thread['inclusive_periods'][key] += sample.period
            thread['inclusive_counts'][key] += 1
        for label, present in (
                ('with_callchain', chain.nr > 0),
                ('unknown_self', '[unknown]' in leaf),
                ('unknown_stack', any('[unknown]' in key for key in inclusive))):
            if present:
                thread['coverage'][label + '_samples'] += 1
                thread['coverage'][label + '_period'] += sample.period
    if not threads:
        raise ReportError('recording contains no samples')
    if not recorded_dsos:
        raise ReportError('no sampled libtotk.so DSO/callchain found; cannot verify its recorded build ID')

    total_period = sum(t['period'] for t in threads.values())
    total_samples = sum(t['sample_count'] for t in threads.values())
    result_threads = []
    for thread in sorted(threads.values(), key=lambda t: (-t['period'], t['pid'], t['tid'])):
        period = thread['period']
        coverage = {}
        for label in ('with_callchain', 'unknown_self', 'unknown_stack'):
            coverage[label + '_samples'] = thread['coverage'][label + '_samples']
            coverage[label + '_period'] = thread['coverage'][label + '_period']
            coverage[label + '_percent'] = percent(coverage[label + '_period'], period)
        result_threads.append({
            'pid': thread['pid'], 'tid': thread['tid'], 'name': thread['name'], 'names': sorted(thread['names']),
            'group': thread_group(thread['name']), 'sample_count': thread['sample_count'], 'period': period,
            'cpu_ms': period / 1e6, 'profile_percent': percent(period, total_period), 'coverage': coverage,
            'self': rankings(thread['self_periods'], thread['self_counts'], period, top),
            'inclusive': rankings(thread['inclusive_periods'], thread['inclusive_counts'], period, top),
        })
    groups = []
    for name in GROUPS:
        members = [t for t in result_threads if t['group'] == name]
        period = sum(t['period'] for t in members)
        groups.append({'name': name, 'threads': [{'pid': t['pid'], 'tid': t['tid']} for t in members],
                       'sample_count': sum(t['sample_count'] for t in members), 'period': period,
                       'cpu_ms': period / 1e6, 'profile_percent': percent(period, total_period)})
    return {'schema': SCHEMA, 'event': event_name, 'period_unit': 'nanoseconds',
            'sample_count': total_samples, 'period': total_period, 'cpu_ms': total_period / 1e6,
            'top': top, 'ranking_percent_denominator': 'thread total period (before top truncation)',
            'symbols': dict(identity, recorded_dsos=recorded_dsos,
                            identity_method='capture-manifest' if expected is None or None in recorded_dsos.values() else 'gnu-build-id'),
            'groups': groups, 'threads': result_threads,
            'limitations': list(LIMITATIONS)}


def build_report(data, symbols, ndk_simpleperf=None, top=20, capture_manifest=None):
    data, symbols = Path(data).resolve(), Path(symbols).resolve()
    if not data.is_file():
        raise ReportError(f'recording does not exist: {data}')
    symbol_file, identity = find_symbols(symbols, allow_missing_id=capture_manifest is not None)
    manifest = (validate_capture_manifest(capture_manifest, data, symbol_file)
                if capture_manifest is not None else None)
    ndk = Path(ndk_simpleperf) if ndk_simpleperf is not None else default_simpleperf_dir()
    factory = load_report_lib(ndk)
    # NDK binary_cache_builder.py documents build_id_list as 0x<20-byte-id>=relative/path.
    # Preserve other supplied cache entries/device-relative paths, but force this ID to
    # our verified unstripped ELF even when the source folder has no cache index.
    with tempfile.TemporaryDirectory(prefix='totk-cpuprof-symbols-') as temporary:
        symfs = Path(temporary)
        # AOSP DebugElfFileFinder also tries symfs/basename(dso_path), including APK
        # internal libtotk.so paths. Both-empty IDs are supported without disabling
        # build-ID checking: android15-release/simpleperf/dso.cpp, FindDebugFile().
        for child in symbols.iterdir():
            if child.name not in ('build_id_list', 'libtotk.so'):
                (symfs / child.name).symlink_to(child, target_is_directory=child.is_dir())
        link_name = 'libtotk.so'
        (symfs / link_name).symlink_to(symbol_file)
        old_index = symbols / 'build_id_list'
        lines = old_index.read_text().splitlines() if old_index.is_file() else []
        if identity['build_id']:
            expected = simpleperf_build_id(identity['build_id'])
            lines = [line for line in lines if line.split('=', 1)[0].lower() != expected]
            lines.append(f'{expected}={link_name}')
        (symfs / 'build_id_list').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        lib = factory()
        try:
            lib.SetSymfs(str(symfs))
            lib.SetRecordFile(str(data))
            result = aggregate_report(lib, identity, top, allow_missing_id=manifest is not None)
        finally:
            lib.Close()
    result.update({'data': str(data), 'symbols_directory': str(symbols), 'ndk_simpleperf': str(ndk.resolve())})
    result['symbols']['file'] = str(symbol_file)
    if manifest is not None:
        assert capture_manifest is not None
        result['capture_manifest'] = dict(manifest, file=str(Path(capture_manifest).resolve()))
        result['limitations'].append(
            'Missing-ID identity relies on the capture manifest attesting the installed APK/commit; '
            'this report verifies recording and symbol hashes, not the device or APK again.')
    return result


def print_summary(report, out):
    print(f"CPU profile: {report['sample_count']} samples, {report['cpu_ms']:.3f} CPU ms ({report['event']}); JSON: {out}")
    print(f"libtotk.so identity: {report['symbols']['identity_method']}")
    for group in report['groups']:
        if group['sample_count']:
            print(f"  {group['name']:9s} {group['cpu_ms']:10.3f} CPU ms  {group['profile_percent']:6.2f}%")
    for thread in report['threads']:
        coverage = thread['coverage']
        print(f"{thread['group']} {thread['pid']}/{thread['tid']} {thread['name']}: {thread['cpu_ms']:.3f} CPU ms, "
              f"{thread['sample_count']} samples; callers {coverage['with_callchain_percent']:.1f}%, "
              f"unknown self {coverage['unknown_self_percent']:.1f}% (period-weighted)")
        for label in ('self', 'inclusive'):
            rows = thread[label][:3]
            print(f"  {label}: " + '; '.join(f"{r['percent']:.1f}% {r['symbol']} ({r['dso']})" for r in rows))
    print('Inclusive is not wall-time critical path; missing symbols/incomplete callchains limit attribution.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('data', type=Path)
    parser.add_argument('--symbols', required=True, type=Path, help='directory containing the exact unstripped libtotk.so')
    parser.add_argument('--ndk-simpleperf', type=Path, help=f'NDK simpleperf scripts directory (default NDK {NDK_VERSION})')
    parser.add_argument('--capture-manifest', type=Path,
                        help='capture JSON binding data/symbol hashes to the installed APK (required for absent GNU IDs)')
    parser.add_argument('--out', type=Path, help='JSON output (default DATA with .json suffix)')
    parser.add_argument('--top', type=int, default=20, help='symbols per thread/ranking (default 20)')
    args = parser.parse_args(argv)
    if args.top < 1:
        parser.error('--top must be positive')
    out = args.out if args.out is not None else args.data.with_suffix('.json')
    if out.resolve() == args.data.resolve():
        parser.error('--out must not overwrite the recording')
    try:
        report = build_report(args.data, args.symbols, args.ndk_simpleperf, args.top, args.capture_manifest)
        out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    except (ReportError, OSError, RuntimeError) as exc:
        print(f'cpuprof_report: {exc}', file=sys.stderr)
        return 1
    print_summary(report, out)
    return 0


if __name__ == '__main__':
    sys.exit(main())

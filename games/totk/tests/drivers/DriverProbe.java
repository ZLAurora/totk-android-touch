package com.aure.totk.setup;

import java.io.File;
import java.io.FileInputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;

/** Persists the label only after the production importer commits, as SetupActivity does with driverName. */
public final class DriverProbe {
    public static void main(String[] args) throws Exception {
        File source = new File(args[0]), target = new File(args[1]);
        File label = new File(target.getParentFile(), "driverName");
        if (args.length > 2 && args[2].equals("directory")) {
            // Exercise the admission check alone: the hostile footer must fail before ZipFile can allocate an index.
            java.lang.reflect.Method check = DriverImport.class.getDeclaredMethod("checkDirectory", File.class);
            check.setAccessible(true);
            try { check.invoke(null, source); System.out.println("directory ok"); }
            catch (java.lang.reflect.InvocationTargetException invalid) {
                if (!(invalid.getCause() instanceof DriverImport.Invalid)) throw invalid;
                System.out.println("invalid zip");
            }
            return;
        }
        InputStream in = new FileInputStream(source);
        if (args.length > 2) {
            String failure = args[2];
            in = new FilterInputStream(in) {
                int reads;
                @Override public int read(byte[] b, int off, int len) throws IOException {
                    if (failure.equals("interrupt") && reads++ > 0) throw new IOException("interrupted source");
                    return super.read(b, off, Math.min(len, 128));
                }
                @Override public void close() throws IOException {
                    super.close();
                    if (failure.equals("close")) throw new IOException("source close failed");
                }
            };
        }
        try {
            String name = DriverImport.install(in, target, new File(target.getParentFile(), "archive.tmp"), source.getName());
            Files.writeString(label.toPath(), name);
            System.out.println("ok " + name);
        } catch (DriverImport.Invalid invalid) {
            System.out.println(invalid.zip ? "invalid zip" : "invalid elf");
        } catch (IOException failed) {
            System.out.println("io " + failed.getMessage());
        }
    }
}

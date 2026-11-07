package com.aure.totk.setup;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Uses the production report builder unchanged on a host JVM. */
public final class SetupDetailsProbe {
    public static void main(String[] args) throws Exception {
        if (args[0].equals("extension")) {
            System.out.print(SetupDetails.extension(args[1]));
            return;
        }
        String metadata = new String(System.in.readAllBytes(), StandardCharsets.UTF_8);
        if (args[0].equals("scrub")) {
            System.out.print(SetupDetails.scrub(metadata, Arrays.asList(Arrays.copyOfRange(args, 1, args.length))));
            return;
        }
        System.out.print(SetupDetails.build(metadata, new File(args[0]), args[1],
                Arrays.asList(Arrays.copyOfRange(args, 2, args.length))));
    }
}

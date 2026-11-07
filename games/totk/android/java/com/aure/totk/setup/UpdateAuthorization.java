package com.aure.totk.setup;

import java.util.UUID;

/** A short-lived, one-use capability within :setup. Only the non-exported entry activity issues it. */
final class UpdateAuthorization {
    private static String token;
    private static long expires;
    private UpdateAuthorization() { }
    static synchronized String issue() {
        token = UUID.randomUUID().toString();
        expires = System.nanoTime() + 30_000_000_000L;
        return token;
    }
    static synchronized boolean consume(String supplied) {
        if (supplied == null || !supplied.equals(token)) return false;
        token = null;
        return System.nanoTime() - expires < 0;
    }
}

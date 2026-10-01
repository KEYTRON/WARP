/*
 * platform.c - which prebuilt packages this machine can run.
 *
 * <os>-<arch> is fixed when warp is built, the C library is not: a static warp
 * binary runs on glibc and on musl alike, so the libc is read from the machine
 * at run time. Linux builds in an index are tagged by libc: `linux-x86_64`
 * (glibc, the default), `linux-x86_64-musl`, and `linux-x86_64-static` that
 * runs anywhere. A machine takes the build for its own libc first and a static
 * one after it; a musl machine never takes a glibc build.
 */
#include "warp.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __APPLE__
#include <dirent.h>
#include <elf.h>

/* The ELF interpreter (PT_INTERP) of a 64-bit little-endian executable, or "". */
static void elf_interp(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum > 64) {
        close(fd);
        return;
    }
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        if (pread(fd, &ph, sizeof(ph), (off_t)(eh.e_phoff + (uint64_t)i * sizeof(ph))) != (ssize_t)sizeof(ph)) break;
        if (ph.p_type != PT_INTERP || ph.p_filesz == 0 || ph.p_filesz >= cap) continue;
        ssize_t n = pread(fd, out, ph.p_filesz, (off_t)ph.p_offset);
        out[n > 0 ? n : 0] = '\0';
        break;
    }
    close(fd);
}

static int has_prefixed_file(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) { found = 1; break; }
    closedir(d);
    return found;
}

#endif /* !__APPLE__ */

const char *warp_libc(void) {
    static const char *libc = NULL;
    if (libc) return libc;
#if defined(__APPLE__)
    return libc = "";
#elif defined(__ANDROID__)
    return libc = "bionic";
#else
    const char *env = getenv("WARP_LIBC");          /* manual override, also what the tests use */
    if (env && (!strcmp(env, "glibc") || !strcmp(env, "musl") || !strcmp(env, "bionic"))) {
        static char forced[16];
        snprintf(forced, sizeof(forced), "%s", env);
        return libc = forced;
    }
    char interp[256];
    elf_interp("/bin/sh", interp, sizeof(interp));
    if (strstr(interp, "ld-musl")) return libc = "musl";
    if (strstr(interp, "ld-linux") || strstr(interp, "ld.so")) return libc = "glibc";
    if (strstr(interp, "linker")) return libc = "bionic";
    /* /bin/sh is static (busybox) or unreadable: look for the loader instead. */
    if (has_prefixed_file("/lib", "ld-musl-") || has_prefixed_file("/usr/lib", "ld-musl-")) return libc = "musl";
    return libc = "glibc";
#endif
}

int warp_platform_candidates(const char **out, int max) {
    static char buf[4][48];
    int n = 0;
    const char *libc = warp_libc();
    const char *os = WARP_OS;
#define ADD(...) do { if (n < max && n < 4) { snprintf(buf[n], sizeof(buf[n]), __VA_ARGS__); out[n] = buf[n]; n++; } } while (0)
    if (strcmp(libc, "musl") == 0) {
        ADD("%s-%s-musl", os, WARP_ARCH);
        ADD("%s-%s-static", os, WARP_ARCH);
    } else if (strcmp(libc, "glibc") == 0) {
        ADD("%s-%s", os, WARP_ARCH);
        ADD("%s-%s-static", os, WARP_ARCH);
    } else if (strcmp(libc, "bionic") == 0) {
        ADD("%s-%s", os, WARP_ARCH);
        ADD("linux-%s-static", WARP_ARCH);          /* a static Linux binary runs on Android's kernel */
    } else {
        ADD("%s-%s", os, WARP_ARCH);                /* macOS */
    }
#undef ADD
    return n;
}

const char *warp_platform(void) {
    const char *c[4];
    return warp_platform_candidates(c, 4) > 0 ? c[0] : WARP_PLATFORM;
}

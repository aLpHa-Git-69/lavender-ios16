/* Static PID 1: inspect HFSX accessibility preferences through XNU, read-only.
 * Boot as the mockfs RAMDisk with rd=md0. /sbin is an existing mount point.
 * No libc, dyld, relocations or filesystem writes are required.
 */
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_select 93
#define SYS_mount 167
#define SYS_unmount 159
#define O_RDONLY 0
#define O_RDWR 2
#define MNT_RDONLY 1

    .text
    .globl _start
    .p2align 2
_start:
    mov x9, sp
    and x9, x9, #~15
    mov sp, x9
    adr x0, console_path
    mov x1, #O_RDWR
    mov x2, #0
    mov x16, #SYS_open
    svc #0x80
    b.cs hang
    mov x19, x0
    adr x1, hello
    adr x2, hello_end
    sub x2, x2, x1
    bl output

    /* hfs_mount_args: fspec pointer followed by zero/default fields. */
    sub sp, sp, #64
    stp xzr, xzr, [sp]
    stp xzr, xzr, [sp, #16]
    stp xzr, xzr, [sp, #32]
    stp xzr, xzr, [sp, #48]
    adr x8, device_path
    str x8, [sp]
    adr x0, fs_type
    adr x1, mount_path
    mov x2, #MNT_RDONLY
    mov x3, sp
    mov x16, #SYS_mount
    svc #0x80
    add sp, sp, #64
    b.cs mount_error
    adr x1, mounted
    adr x2, mounted_end
    sub x2, x2, x1
    bl output

    adr x20, access_path
    bl inspect
    adr x20, media_path
    bl inspect
    adr x20, collection_path
    bl inspect

    adr x0, mount_path
    mov x1, #0
    mov x16, #SYS_unmount
    svc #0x80
    b.cs mount_error
    adr x1, done
    adr x2, done_end
    sub x2, x2, x1
    bl output
    b idle

mount_error:
    mov x24, x0
    adr x1, mount_failed
    adr x2, mount_failed_end
    sub x2, x2, x1
    bl output
    mov x0, x24
    bl hex_error
    b idle

/* Inspect one file, printing each byte as hex for later plist decoding. */
inspect:
    stp x29, x30, [sp, #-16]!
    mov x1, x20
    mov x2, #0
len_path:
    ldrb w8, [x1, x2]
    cbz w8, path_ready
    add x2, x2, #1
    b len_path
path_ready:
    bl output
    adr x1, newline
    mov x2, #1
    bl output
    mov x0, x20
    mov x1, #O_RDONLY
    mov x2, #0
    mov x16, #SYS_open
    svc #0x80
    b.cs file_error
    mov x21, x0
    sub sp, sp, #3072       /* 1024 input bytes, 2048 hex output */
    mov x22, #0
read_chunk:
    mov x0, x21
    mov x1, sp
    mov x2, #1024
    mov x16, #SYS_read
    svc #0x80
    b.cs read_error
    cbz x0, close_file
    add x22, x22, x0
    mov x23, x0
    mov x3, #0
    add x4, sp, #1024
    adr x5, digits
hex_bytes:
    ldrb w6, [sp, x3]
    lsr w7, w6, #4
    ldrb w7, [x5, x7]
    strb w7, [x4], #1
    and w6, w6, #15
    ldrb w6, [x5, x6]
    strb w6, [x4], #1
    add x3, x3, #1
    cmp x3, x23
    b.lo hex_bytes
    add x1, sp, #1024
    lsl x2, x23, #1
    bl output
    adr x1, newline
    mov x2, #1
    bl output
    cmp x22, #8192
    b.lo read_chunk
    b close_file
read_error:
    bl hex_error
close_file:
    mov x0, x21
    mov x16, #SYS_close
    svc #0x80
    add sp, sp, #3072
    b inspect_end
file_error:
    bl hex_error
inspect_end:
    ldp x29, x30, [sp], #16
    ret

/* Report errno as eight hex digits. */
hex_error:
    stp x29, x30, [sp, #-32]!
    adr x5, digits
    add x1, sp, #16
    mov x6, #28
    mov x7, #0
error_digit:
    lsr w8, w0, w6
    and w8, w8, #15
    ldrb w8, [x5, x8]
    strb w8, [x1, x7]
    add x7, x7, #1
    sub x6, x6, #4
    cmp x7, #8
    b.lo error_digit
    mov x2, #8
    bl output
    adr x1, newline
    mov x2, #1
    bl output
    ldp x29, x30, [sp], #32
    ret

output:
    mov x0, x19
    mov x16, #SYS_write
    svc #0x80
    ret
idle:
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    adr x4, five_sec
    mov x16, #SYS_select
    svc #0x80
    b idle
hang:
    wfe
    b hang

    .p2align 3
five_sec:
    .quad 5, 0
digits:
    .ascii "0123456789abcdef"
newline:
    .ascii "\n"
console_path:
    .asciz "/dev/console"
device_path:
    .asciz "/dev/emmcroot0"
fs_type:
    .asciz "hfs"
mount_path:
    .asciz "/sbin"
hello:
    .ascii "\nLAVENDER RESCUE: read-only accessibility inspection\n"
hello_end:
mounted:
    .ascii "LAVENDER RESCUE: HFSX mounted read-only at /sbin\n"
mounted_end:
done:
    .ascii "LAVENDER RESCUE: inspection complete, HFSX unmounted\n"
done_end:
mount_failed:
    .ascii "LAVENDER RESCUE: mount/unmount errno 0x"
mount_failed_end:
access_path:
    .asciz "/sbin/private/var/mobile/Library/Preferences/com.apple.Accessibility.plist"
media_path:
    .asciz "/sbin/private/var/mobile/Library/Preferences/com.apple.mediaaccessibility.plist"
collection_path:
    .asciz "/sbin/private/var/mobile/Library/Preferences/com.apple.Accessibility.Collection.plist"

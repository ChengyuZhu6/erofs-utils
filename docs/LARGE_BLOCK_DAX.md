# EROFS large-block DAX prototype

Use an explicit block size to share an uncompressed image among Linux guests
with different page sizes. This branch defaults to a compiled-in maximum of
64 KiB on macOS and Linux. The default *image* block size still follows the
build host's page size, capped by that maximum. `-b4096` works on macOS;
`-b16384` and `-b65536` now work in a normal Linux build without a special
`MAX_BLOCK_SIZE` configure setting.

## Build and create an image

This branch is based on erofs-utils commit
`de2dbe430f066ab02d7272e6b47fcc70446bca61`. Follow [INSTALL.md](INSTALL.md)
for dependencies, then build and select the shared image's block size:

```sh
./autogen.sh
./configure --disable-fuse
make -j8
mkfs/mkfs.erofs -b65536 -E^inline_data shared.erofs rootfs/
fsck/fsck.erofs shared.erofs
```

The matching Linux `hudson/dax` branch is based on 7.4-rc2, commit
`7475ea8b58616b33c04afb976cf0fbaad9dd63fb`. It enables blocks larger than the
guest page size, up to the block layer's 64 KiB maximum with
`CONFIG_TRANSPARENT_HUGEPAGE=y`. A stock kernel that rejects
`blkszbits 14 isn't supported` still needs this kernel change.

Use 16 KiB blocks for 4/16 KiB-page clients, or 64 KiB blocks when selecting a
common image block size up to 64 KiB. The validated guests have 4 KiB and
16 KiB pages. For FSDAX, use plain files as above, expose the image through a
DAX-capable PMEM device, and mount in the Linux guest:

```sh
mkdir -p /mnt/erofs
mount -t erofs -o ro,dax=always /dev/pmem0 /mnt/erofs
```

Check `STATX_ATTR_DAX` on the file; a successful mount alone does not prove
FSDAX. Loop devices do not provide FSDAX. Buffered file-backed use is also
supported with `mount -t erofs -o ro,dax=never shared.erofs /mnt/erofs`.
macOS is the build host in these tests; mounting and FSDAX run inside Linux.

When block size exceeds PAGE_SIZE, this prototype rejects compression,
metaboxes and external-device tables with `EOPNOTSUPP`. Inline and chunk
files use buffered access. When block size is smaller than PAGE_SIZE,
upstream's DAX restriction remains. The earlier 4 KiB-image/16 KiB-page
sub-page-DAX kernel prototype is a separate patch series.

## Padding protection

Plain files reserve and zero their complete data alignment unit before
metadata allocation.  Formatter errors are propagated to the caller.
`--dsunit` is unnecessary when the filesystem block size already meets the
page alignment requirement.

## End-to-end validation

The kernel repository contains `tools/testing/selftests/filesystems/erofs_dax.c`
and `erofs_lbs.py`. The complete build, initramfs and QEMU commands are in
`Documentation/filesystems/erofs-large-blocks.rst` on the matching Linux branch.
Generate an independent fixture set on each build OS:

```sh
python3 /path/to/linux/tools/testing/selftests/filesystems/erofs_lbs.py \
    "$PWD/mkfs/mkfs.erofs" "$PWD/dump/dump.erofs" /tmp/erofs-fixtures
python3 scripts/test-dax-padding.py \
    "$PWD/mkfs/mkfs.erofs" "$PWD/dump/dump.erofs" "$PWD/fsck/fsck.erofs"
```

The fixture destination must be new. Enable LZ4 in mkfs for the compression
regressions. Large fixture xattrs are supplied through PAX tar headers, so
the source filesystem's xattr size limit does not constrain the tests.
Transfer each set unchanged to both guests. Differences in source metadata,
such as symlink permissions, can produce different hashes between independent
macOS and Linux builds; each individual image is reused unchanged across guests.

The fixtures cover 4/16/64 KiB blocks, plain/inline/chunk layouts, PMEM and
file-backed access, inode_share, cross-page metadata, EOF zeros, SIGBUS and
corrupt-image rejection.  The padding regression checks stale output,
neighboring allocations and extraction with one and four workers, and
verifies that invalid block sizes leave existing output unchanged.

The supported scope is uncompressed, single-device images.  Real hardware
PMEM, performance and 64 KiB-page guests remain unvalidated.

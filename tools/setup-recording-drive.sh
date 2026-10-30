#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Prepare a USB recording volume and configure systemd automounting.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: sudo setup-recording-drive.sh --device DEVICE
       [--owner USER] [--mountpoint /mnt/sippymodem-recordings]
       [--dry-run] [--format-ext4 | --repartition-ext4]

Preserves existing exFAT/ext4 data unless an erase option is explicitly supplied.
--format-ext4 ERASES the selected partition; other partitions are untouched.
--repartition-ext4 requires a WHOLE USB DISK and ERASES ALL its partitions.
It creates GPT with one Linux partition occupying the available disk, then
formats it as ext4. An old partition-table dump is saved in /var/backups.
Other modes require a partition, such as /dev/disk/by-id/...-partN.
Uses a UUID in /etc/fstab, backs up fstab before changes, and enables an
on-demand mount at boot. No recordings are moved and no modem jobs are resumed.
EOF
}
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
# A direct automount and its mounted filesystem can share one mountpoint.
mounted_uuid() {
    findmnt -rn -M "$target" -o FSTYPE,UUID | while read -r kind volume_uuid; do
        [[ "$kind" == autofs ]] || printf '%s\n' "${volume_uuid:--}"
    done
}
device='' owner=${SUDO_USER:-} target=/mnt/sippymodem-recordings
dry_run=0 format=0 repartition=0
while (($#)); do
    case "$1" in
        --device|--owner|--mountpoint)
            (($# >= 2)) || die "$1 requires a value"
            case "$1" in
                --device) device=$2 ;; --owner) owner=$2 ;; --mountpoint) target=$2 ;;
            esac
            shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        --format-ext4) format=1; shift ;;
        --repartition-ext4) repartition=1; format=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument: $1" ;;
    esac
done
[[ -n "$device" && -n "$owner" ]] || { usage >&2; exit 1; }
for command in lsblk readlink realpath blkid findmnt mountpoint id python3 systemctl systemd-escape; do
    command -v "$command" >/dev/null || die "Missing command: $command"
done
device=$(readlink -f -- "$device")
[[ -b "$device" ]] || die "Not a block device: $device"
if ((repartition)); then
    [[ $(lsblk -dnro TYPE -- "$device") == disk ]] || die '--repartition-ext4 requires a whole disk'
    [[ $(lsblk -dnro TRAN -- "$device") == usb ]] || die 'Selected disk is not USB'
    command -v sfdisk >/dev/null || die 'Missing sfdisk'
    [[ -z $(lsblk -nro MOUNTPOINTS -- "$device") ]] || die 'Unmount all disk partitions, including swap, before repartitioning'
    while read -r kind; do
        [[ "$kind" == disk || "$kind" == part ]] || die 'Disk has dependent block devices'
    done < <(lsblk -nro TYPE -- "$device")
else
    [[ $(lsblk -dnro TYPE -- "$device") == part ]] || die 'Select a partition, or explicitly use --repartition-ext4 for a whole disk'
    parent=$(lsblk -dnro PKNAME -- "$device")
    [[ -n "$parent" && $(lsblk -dnro TRAN -- "/dev/$parent") == usb ]] || die 'Selected partition is not on a USB disk'
fi
[[ $(lsblk -dnro RO -- "$device") == 0 ]] || die 'Selected device is read-only'
owner_uid=$(id -u -- "$owner")
owner_gid=$(id -g -- "$owner")
target=${target%/}
case "$target" in /mnt/*|/media/*) ;; *) die 'Mountpoint must be below /mnt or /media' ;; esac
case "$target" in *[[:space:]]*|*\\*) die 'Mountpoint cannot contain whitespace or backslashes' ;; esac
[[ $(realpath -m -- "$target") == "$target" ]] || die 'Use a canonical mountpoint without symlinks or ..'
[[ ! -L "$target" ]] || die 'Mountpoint cannot be a symlink'
filesystem=$(lsblk -dnro FSTYPE -- "$device")
uuid=$(lsblk -dnro UUID -- "$device")
mounted_at=$(findmnt -rn -S "$device" -o TARGET || true)
if [[ -n "$mounted_at" && "$mounted_at" != "$target" ]]; then
    die "Device is already mounted at $mounted_at"
fi
existing_uuid=$(mounted_uuid || true)
if [[ -n "$existing_uuid" ]]; then
    [[ "$existing_uuid" == "$uuid" ]] || die 'Mountpoint is occupied by another filesystem'
fi
if ((format)); then
    ! mountpoint -q -- "$target" || die 'Stop the existing mount/automount before formatting'
    [[ -z $(lsblk -nro MOUNTPOINTS -- "$device") ]] || die 'Unmount the selected partition before formatting'
    if ((!repartition)); then
        [[ $(lsblk -nro NAME -- "$device" | wc -l) == 1 ]] || die 'Device has dependent block devices'
    fi
    command -v mkfs.ext4 >/dev/null || die 'Missing mkfs.ext4'
    command -v udevadm >/dev/null || die 'Missing udevadm'
elif [[ "$filesystem" != exfat && "$filesystem" != ext4 ]]; then
    die 'Existing filesystem must be exFAT/ext4; formatting requires --format-ext4'
fi
[[ -n "$uuid" || "$format" == 1 ]] || die 'Filesystem has no UUID'
if [[ -d "$target" ]] && ! mountpoint -q -- "$target"; then
    [[ -z $(find "$target" -mindepth 1 -maxdepth 1 -print -quit) ]] || die 'Unmounted mountpoint contains files'
fi

# Only replace our marked stanza. Refuse conflicting, unmanaged entries.
protected_sources=()
if ((repartition)); then
    while read -r child; do
        protected_sources+=("$child")
        child_uuid=$(lsblk -dnro UUID -- "$child")
        [[ -z "$child_uuid" ]] || protected_sources+=("UUID=$child_uuid")
        references=$(findmnt --fstab --evaluate -rn -S "$child" -o TARGET || true)
        [[ -z "$references" || "$references" == "$target" ]] || die "Disk is referenced by fstab at $references"
    done < <(lsblk -nrpo PATH -- "$device")
fi
fstab_entry() {
    python3 - "$1" "$target" "$device" "$2" "$3" "$4" "${protected_sources[@]}" <<'PY'
from pathlib import Path
import os, shutil, subprocess, sys, tempfile, time
mode, target, device, uuid, filesystem, options = sys.argv[1:7]
protected = set(sys.argv[7:]) | {device, 'UUID=' + uuid}
path = Path('/etc/fstab')
old = path.read_text()
begin, end = '# BEGIN sippymodem recording drive', '# END sippymodem recording drive'
lines = old.splitlines(keepends=True)
starts = [i for i, s in enumerate(lines) if s.strip() == begin]
ends = [i for i, s in enumerate(lines) if s.strip() == end]
if len(starts) != len(ends) or len(starts) > 1 or (starts and starts[0] >= ends[0]):
    sys.exit('Malformed existing recording-drive stanza; fstab unchanged')
outside = lines if not starts else lines[:starts[0]] + lines[ends[0]+1:]
for line in outside:
    fields = line.split()
    if fields and not fields[0].startswith('#') and len(fields) >= 2:
        if fields[1] == target or fields[0] in protected:
            sys.exit('Conflicting unmanaged fstab entry; fstab unchanged')
if starts:
    for line in lines[starts[0]+1:ends[0]]:
        fields = line.split()
        if fields and not fields[0].startswith('#') and len(fields) >= 2 and fields[1] != target:
            sys.exit('Existing managed drive uses another mountpoint; fstab unchanged')
if mode == 'check':
    sys.exit(0)
stanza = f'{begin}\nUUID={uuid} {target} {filesystem} {options} 0 0\n{end}\n'
new = ''.join(outside).rstrip('\n') + '\n\n' + stanza
if new == old:
    sys.exit(0)
fd, temporary = tempfile.mkstemp(prefix='.fstab-sippymodem-', dir=path.parent)
try:
    with os.fdopen(fd, 'w') as output:
        output.write(new)
        output.flush()
        os.fsync(output.fileno())
    shutil.copystat(path, temporary)
    subprocess.run(['findmnt', '--verify', '--tab-file', temporary], check=True)
    if path.read_text() != old:
        sys.exit('fstab changed concurrently; refusing replacement')
    backup = path.with_name(f'fstab.sippymodem-backup-{time.time_ns()}')
    shutil.copy2(path, backup)
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    print(f'fstab backup: {backup}')
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
PY
}
options='nofail,noatime,x-systemd.automount,x-systemd.device-timeout=5s,x-systemd.mount-timeout=30s'
[[ "$filesystem" != exfat ]] || options+=",uid=$owner_uid,gid=$owner_gid,fmask=0177,dmask=0077"
fstab_entry check "$uuid" "$filesystem" "$options"
printf 'Device: %s\nCurrent filesystem: %s\nMountpoint: %s\nRecording owner: %s (%s:%s)\n' "$device" "${filesystem:-none on this device}" "$target" "$owner" "$owner_uid" "$owner_gid"
if ((dry_run)); then
    if ((repartition)); then
        printf 'Would ERASE ALL partitions on %s, create one GPT Linux partition, and format it as ext4.\n' "$device"
    elif ((format)); then
        printf 'Would ERASE %s and create ext4 with a new UUID.\n' "$device"
    fi
    printf 'Would configure UUID automount and create %s/sippymodem. No changes made.\n' "$target"
    exit 0
fi
((EUID == 0)) || die 'Run with sudo, or use --dry-run'
[[ $(cat /proc/1/comm) == systemd ]] || die 'This setup requires systemd as PID 1'
for command in install runuser; do command -v "$command" >/dev/null || die "Missing command: $command"; done
if ((repartition)); then
    backup_dir=$(mktemp -d /var/backups/sippymodem-drive.XXXXXX)
    if [[ -n $(lsblk -dnro PTTYPE -- "$device") ]]; then
        sfdisk --dump "$device" > "$backup_dir/partition-table.sfdisk"
    else
        printf '# No recognized existing partition table\n' > "$backup_dir/partition-table.sfdisk"
    fi
    printf 'Partition-table backup (not a data backup): %s\n' "$backup_dir"
    printf 'label: gpt\n\ntype=L\n' | sfdisk --lock=yes --wipe always --wipe-partitions always "$device"
    case "$device" in *[0-9]) device+=p1 ;; *) device+=1 ;; esac
    udevadm settle --timeout=30 --exit-if-exists="$device"
    [[ -b "$device" ]] || die 'New partition device did not appear'
    [[ $(lsblk -dnro TYPE -- "$device") == part ]] || die 'New device is not a partition'
fi
if ((format)); then
    mkfs.ext4 -F -m 0 -L sippymodem-data "$device"
    udevadm trigger --action=change --subsystem-match=block --sysname-match="${device##*/}"
    udevadm settle --timeout=30
    filesystem=ext4
    uuid=$(blkid -p -s UUID -o value -- "$device")
    options='nofail,noatime,x-systemd.automount,x-systemd.device-timeout=5s,x-systemd.mount-timeout=30s'
fi
if ! mountpoint -q -- "$target"; then
    install -d -o root -g root -m 0700 -- "$target"
fi
fstab_entry write "$uuid" "$filesystem" "$options"
systemctl daemon-reload
systemctl start "$(systemd-escape --path --suffix=automount "$target")"
systemctl start "$(systemd-escape --path --suffix=mount "$target")"
[[ $(mounted_uuid) == "$uuid" ]] || die 'Mounted UUID does not match the selected drive'
recordings=$target/sippymodem
[[ ! -L "$recordings" ]] || die 'Recording directory cannot be a symlink'
if [[ "$filesystem" == exfat ]]; then
    mkdir -p -- "$recordings"
else
    install -d -o "$owner_uid" -g "$owner_gid" -m 0700 -- "$recordings"
fi
runuser -u "$owner" -- python3 - "$recordings" <<'PY'
import os, sys, tempfile
fd, path = tempfile.mkstemp(prefix='.write-test-', dir=sys.argv[1])
try:
    os.write(fd, b'recording drive write test\n')
    os.fsync(fd)
finally:
    os.close(fd)
    os.unlink(path)
PY
printf 'Ready: %s\nNo recordings were moved; modem jobs were not changed.\n' "$recordings"

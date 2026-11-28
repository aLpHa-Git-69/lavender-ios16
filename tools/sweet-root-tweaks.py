#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py MOUNTED_ROOT

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
turned the plain mkiosroot.sh output into the images that reach the home
screen (image versions v8-v14 in NOTES.md):

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); CommCenter jobs removed (no baseband: it
                  crash-loops and Setup blocks on its CoreTelephony XPC)
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark seeded; the actual Activated state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py
"""
import glob, os, plistlib, shutil, sys, zipfile

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    parent = os.path.dirname(path)
    if not os.path.isdir(parent):
        os.makedirs(parent)
        os.chown(parent, MOBILE, MOBILE)
        os.chmod(parent, 0o755)
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.CommCenter']
    for k in removed:
        del jobs[k]
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} CommCenter jobs removed')


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    save(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('data ark', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


def staged_apps(root):
    """Promote /private/var/staged_system_apps (.app or .ipa) into /Applications.

    iOS normally installs these during DataMigration on first boot; skipping
    DataMigration leaves only the ~6 pre-installed apps visible on SpringBoard.
    """
    staged = os.path.join(root, 'private/var/staged_system_apps')
    apps_dir = os.path.join(root, 'Applications')
    if not os.path.isdir(staged):
        print('staged_system_apps: not present')
        return
    os.makedirs(apps_dir, exist_ok=True)
    promoted = []
    for entry in sorted(os.listdir(staged)):
        src = os.path.join(staged, entry)
        if entry.endswith('.app') and os.path.isdir(src):
            dst = os.path.join(apps_dir, entry)
            if not os.path.exists(dst):
                shutil.move(src, dst)
                promoted.append(entry)
        elif entry.endswith('.ipa') and os.path.isfile(src):
            with zipfile.ZipFile(src) as zf:
                app_names = sorted({
                    parts[1] for name in zf.namelist()
                    for parts in [name.split('/')]
                    if len(parts) >= 2 and parts[0] == 'Payload' and parts[1].endswith('.app')
                })
                for app_name in app_names:
                    dst_app = os.path.join(apps_dir, app_name)
                    if os.path.exists(dst_app):
                        continue
                    prefix = f'Payload/{app_name}/'
                    for info in zf.infolist():
                        if not info.filename.startswith(prefix):
                            continue
                        rel = info.filename[len(prefix):]
                        target = os.path.join(dst_app, rel) if rel else dst_app
                        mode = (info.external_attr >> 16) & 0o7777
                        if info.is_dir():
                            os.makedirs(target, exist_ok=True)
                            os.chown(target, 0, 0)
                            os.chmod(target, mode or 0o755)
                        elif (info.external_attr >> 16) & 0o170000 == 0o120000:
                            os.makedirs(os.path.dirname(target), exist_ok=True)
                            if not os.path.lexists(target):
                                os.symlink(zf.read(info).decode('utf-8'), target)
                                os.lchown(target, 0, 0)
                        else:
                            os.makedirs(os.path.dirname(target), exist_ok=True)
                            with zf.open(info) as zin, open(target, 'wb') as zout:
                                shutil.copyfileobj(zin, zout)
                            os.chown(target, 0, 0)
                            os.chmod(target, mode or 0o644)
                    promoted.append(app_name)
            os.unlink(src)
    print(f'staged_system_apps: promoted {len(promoted)} apps -> /Applications ({", ".join(promoted[:12])}{"..." if len(promoted) > 12 else ""})')


def files_and_wallpaper(root):
    """Enable local Files directories and native PosterBoard/Wallpaper extension discovery."""
    for rel, uid, gid, mode in (
        ('private/var/mobile/Documents', MOBILE, MOBILE, 0o755),
        ('private/var/mobile/tmp', MOBILE, MOBILE, 0o700),
        ('private/var/tmp/lavmetal-cache', 0, 0, 0o1777),
    ):
        d = os.path.join(root, rel)
        os.makedirs(d, exist_ok=True)
        os.chown(d, uid, gid)
        os.chmod(d, mode)

    # Set aside the AMFI-gated precompiled extension catalog so lsd scans natively.
    ext_plist = os.path.join(root, 'System/Library/xpc/extensions.plist')
    ext_bak = ext_plist + '.stock'
    if os.path.isfile(ext_plist) and not os.path.exists(ext_bak):
        shutil.move(ext_plist, ext_bak)
        print('extensions.plist moved aside -> extensions.plist.stock (native lsd extension scan)')

    # Register com.apple.posterkit.provider.appexpt for iOS (platform 2) from PosterKit.framework.
    pk_plist = os.path.join(root, 'System/Library/PrivateFrameworks/PosterKit.framework/Info.plist')
    ep_dir = os.path.join(root, 'System/Library/ExtensionKit/ExtensionPoints')
    ep_out = os.path.join(ep_dir, 'com.apple.posterkit.provider.appexpt')
    if os.path.isfile(pk_plist) and not os.path.exists(ep_out):
        pk_info = load(pk_plist)
        sdk_root = pk_info.get('NSExtensionSDK', {})
        sdk = sdk_root.get('com.apple.posterkit.provider', sdk_root) if isinstance(sdk_root, dict) else {}
        if sdk:
            os.makedirs(ep_dir, exist_ok=True)
            # Match the exact nesting used by existing sibling .appexpt files in this iOS build.
            payload = {'com.apple.posterkit.provider': {'2': sdk}}
            for peer in sorted(glob.glob(os.path.join(ep_dir, '*.appexpt'))):
                try:
                    sample = load(peer)
                    if isinstance(sample, dict) and sample:
                        first_v = next(iter(sample.values()))
                        if isinstance(first_v, dict) and any(k in first_v for k in ('2', 2, '1', 1)):
                            plat_key = 2 if any(isinstance(k, int) for k in first_v) else '2'
                            payload = {'com.apple.posterkit.provider': {plat_key: sdk}}
                        elif '2' in sample or 2 in sample:
                            plat_key = 2 if 2 in sample else '2'
                            payload = {plat_key: sdk}
                        else:
                            payload = sdk
                        break
                except Exception:
                    pass
            save(ep_out, payload, 0, 0, 0o644)
            print('posterkit appexpt written:', ep_out, 'top keys:', list(payload.keys()))


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], 'System/Library/xpc')):
        sys.exit(__doc__)
    root = sys.argv[1]
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    staged_apps(root)
    files_and_wallpaper(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.DataMigration', {'LastSystemVersion': BUILD})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()

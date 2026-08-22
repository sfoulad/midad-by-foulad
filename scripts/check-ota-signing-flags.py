"""
Fail-closed guard: every `[env:*gh_release]` / `[env:*gh_release_rc]` section in
platformio.ini must define -DOTA_SIGNING_BOOT_CHECK_ENABLED=1 in its own
build_flags. See src/OtaSigningBootGuard.cpp -- without this flag, the
boot-time app-signature check silently no-ops for that release build.

A new board's release/RC env can be added without ever hitting a merge
conflict on the existing envs (see platformio.ini's OTA_SIGNING_BOOT_CHECK_ENABLED
comment), which is exactly how the x4pro/papermono release envs shipped without
it once already. This script exists so that omission fails CI instead of
shipping a release with the check silently disabled.
"""

import configparser
import sys
from pathlib import Path

FLAG = '-DOTA_SIGNING_BOOT_CHECK_ENABLED=1'


def is_release_env(section_name):
    if not section_name.startswith('env:'):
        return False
    env_name = section_name[len('env:'):]
    return env_name.endswith('gh_release') or env_name.endswith('gh_release_rc')


def main():
    ini_path = Path(__file__).resolve().parent.parent / 'platformio.ini'
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')

    release_sections = [s for s in config.sections() if is_release_env(s)]
    if not release_sections:
        print('ERROR: no *gh_release/*gh_release_rc sections found in platformio.ini '
              '-- the section-matching pattern itself may be broken', file=sys.stderr)
        return 1

    missing = []
    for section in release_sections:
        build_flags = config.get(section, 'build_flags', fallback='')
        if FLAG not in build_flags:
            missing.append(section)

    if missing:
        print(f'ERROR: {FLAG} is missing from these release/RC environments:', file=sys.stderr)
        for section in missing:
            print(f'  [{section}]', file=sys.stderr)
        print('Add it to build_flags, matching every other *gh_release*/env\'s '
              'existing OTA_SIGNING_BOOT_CHECK_ENABLED comment. See '
              'src/OtaSigningBootGuard.cpp for why this matters.', file=sys.stderr)
        return 1

    print(f'OK: {FLAG} present in all {len(release_sections)} release/RC environments: '
          f'{", ".join(s[len("env:"):] for s in release_sections)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

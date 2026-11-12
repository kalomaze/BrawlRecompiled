#!/usr/bin/env python3
"""Build driver for Linux and Windows. All mutable state belongs to the build directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SDK = '8.0.425'
# Keyed by .NET runtime identifier; Windows ships the SDK as a .zip, Linux as a .tar.gz.
SDK_HASHES = {
    'linux-x64': '934b8060a7190e5909ad1fd0785db542f487b3bbf6cdd14826b02095fdd0d0394298b1634085eff302928fccc33f7c1a7253e9b87df555fc36fce819bcd2e798',
    'linux-arm64': '84a4d017d74d7aa842e981679d1b044e6be1f35b9b2b214021e30bc40871d016d29611cca373d8502ad5c890e3ad360ee698f9cf2d7a4d5a9fba102d88ba310f',
    'win-x64': 'f0b6f15bf6f1a0507205c0cb102ab99e1dee875c4682c8ed94665be1d580186a06b21455e83b3a01a0ff7f4cd887b67420f2e2fe09ed985534a4cea488ae1af9',
    'win-arm64': '2a75864daac54fe8360498d0a7260e3f8d851c3d811d864d7229f00ae51eac9e534998b7c3d221fc423319b9c8147dd43fa80690c969e43b2045cc2f1f93e537',
}
DECOMP = '70744045f8ceba62d5a77c052c8f7bcb76fc21d1'


def run(*args, cwd=None):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def download(url, path, digest=None, algorithm='sha256'):
    if not path.exists():
        print(f'Downloading {url}', flush=True)
        temporary = path.with_suffix('.partial')
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(path)
    if digest:
        with path.open('rb') as stream:
            actual = hashlib.file_digest(stream, algorithm).hexdigest()
        if actual != digest:
            path.unlink()
            raise RuntimeError(f'Checksum mismatch: {path}; retry to download again')


def unpack(archive, destination):
    if archive.suffix == '.zip':
        shutil.unpack_archive(archive, destination)  # zipfile drops absolute and .. members itself
    else:
        shutil.unpack_archive(archive, destination, filter='data')


def translation_fingerprint(sources):
    # The translator reads native registrations/effects from runtime/src, not the
    # renderer or runtime headers. Native CMake/Ninja tracks those separately.
    inputs = {name: digest for name, digest in sources.items()
              if name.startswith(('wiicompiled/translator/src/',
                                  'wiicompiled/projects/brawl/',
                                  'wiicompiled/runtime/src/')) or
              name == 'tools/build.py'}
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=ROOT / 'Disc/DATA',  # where the launchers extract too
                        help='extracted disc: built from, and played')
    parser.add_argument('--extract', type=Path, help='disc image to extract to --game, instead of building')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--native-arg', action='append', default=[],
                        help='extra argument for the native CMake configure; repeatable')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    game, build = args.game.resolve(), args.build_dir.resolve()
    if build == ROOT or ROOT.is_relative_to(build):
        parser.error('build directory must not contain the source checkout')
    build.mkdir(parents=True, exist_ok=True)
    # A second build must not overwrite extracted inputs while the first uses them.
    with (build / 'build.lock').open('w') as lock:
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        build_game(args, game, build)


def build_game(args, game, build):
    for tool in ('git', 'clang', 'clang++', 'ninja'):
        if not shutil.which(tool):
            raise RuntimeError(f'Missing {tool} on PATH. See "Building from source" in README.md for the prerequisites.')
    downloads = build / 'downloads'
    downloads.mkdir(exist_ok=True)
    arch = {'x86_64': 'x64', 'amd64': 'x64', 'aarch64': 'arm64', 'arm64': 'arm64'}[platform.machine().lower()]
    rid = f'{"win" if os.name == "nt" else "linux"}-{arch}'
    dotnet_dir = build / 'dotnet'
    sdk_stamp = dotnet_dir / '.sdk-ready'
    if not sdk_stamp.exists():
        name = f'dotnet-sdk-{SDK}-{rid}.{"zip" if os.name == "nt" else "tar.gz"}'
        archive = downloads / name
        download(f'https://builds.dotnet.microsoft.com/dotnet/Sdk/{SDK}/{name}', archive, SDK_HASHES[rid], 'sha512')
        staging = build / 'dotnet.partial'
        if staging.exists():
            shutil.rmtree(staging)
        unpack(archive, staging)
        (staging / '.sdk-ready').write_text(SDK)
        if dotnet_dir.exists():
            shutil.rmtree(dotnet_dir)
        staging.rename(dotnet_dir)
    dotnet = shutil.which('dotnet', path=dotnet_dir)  # dotnet.exe on Windows
    os.environ.update(DOTNET_ROOT=str(dotnet_dir), DOTNET_CLI_HOME=str(build / 'dotnet-home'),
                      NUGET_PACKAGES=str(build / 'nuget'), DOTNET_CLI_TELEMETRY_OPTOUT='1',
                      DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_NOLOGO='1')
    # Track source hashes separately from staged files, which upstream generators modify.
    # Preserve generated code and timestamps so Ninja can reuse unchanged objects.
    source_files = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-co', '--exclude-standard', '-z']).split(b'\0')
    source_files = [os.fsdecode(raw) for raw in source_files if raw and
                    not (ROOT / os.fsdecode(raw)).is_relative_to(build) and
                    (raw.startswith((b'wiicompiled/', b'tools/disc-extract/')) or
                     raw in (b'launcher/WiiDisc.cs', b'tools/build.py'))]
    work = build / 'source'
    source_stamp = build / 'sources.json'
    previous = json.loads(source_stamp.read_text()) if source_stamp.exists() else {}
    current = {}
    for name in sorted(source_files):
        source, target = ROOT / name, work / name
        if not source.is_file():
            continue
        current[name] = hashlib.sha256(source.read_bytes()).hexdigest()
        if current[name] != previous.get(name) or not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != current[name]:
                # New content needs a new mtime: copying an older checkout timestamp
                # can leave Ninja using a Clang precompiled header for stale contents.
                shutil.copyfile(source, target)
    for name in previous.keys() - current.keys():
        (work / name).unlink(missing_ok=True)
    source_stamp.write_text(json.dumps(current, sort_keys=True))
    fingerprint = translation_fingerprint(current)
    wc = work / 'wiicompiled'
    tool = 'tools/disc-extract/DiscExtract' if args.extract else 'wiicompiled/translator/src/Translator.Cli/Translator.Cli'
    run(dotnet, 'build', work / f'{tool}.csproj', '-c', 'Release', '--nologo', '--disable-build-servers')
    if args.extract:
        if game.exists():
            raise RuntimeError(f'{game} already exists; delete it to extract again')
        partial = game.with_name(game.name + '.partial')  # rerun to resume: the extractor overwrites
        run(dotnet, work / 'tools/disc-extract/bin/Release/net8.0/DiscExtract.dll', args.extract.resolve(strict=True), partial)
        partial.rename(game)
        print(f'Extracted: {game}', flush=True)
        return
    translator = wc / 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
    dol = game / 'sys/main.dol'
    if not dol.is_file():
        raise RuntimeError(f'No extracted disc in {game}: run extract.cmake first, or pass -DGAME=<folder>')
    game_inputs = hashlib.sha256()
    for path in (dol, *sorted((game / 'files/module').iterdir())):
        game_inputs.update(path.name.encode() + path.read_bytes())
    decomp = build / f'brawl-{DECOMP}'
    if not (decomp / '.ready').exists():
        archive = downloads / f'brawl-{DECOMP}.tar.gz'
        download(f'https://codeload.github.com/doldecomp/brawl/tar.gz/{DECOMP}', archive,
                 '1ae548ca5d22859e9475d3af47bc2b63a33a63c16d9e0ad61d2579b70b9806b8')
        unpack(archive, build)
        (decomp / '.ready').touch()
    translated_stamp = build / 'translated.json'
    translation_key = json.dumps([fingerprint, game_inputs.hexdigest()])
    if not translated_stamp.exists() or translated_stamp.read_text() != translation_key:
        inputs = work / 'game'
        inputs.mkdir(exist_ok=True)
        shutil.copy2(dol, inputs / 'main.dol')
        run(sys.executable, wc / 'projects/brawl/tools/build_modules.py', game / 'files/module',
            decomp / 'config/RSBE01_02', inputs / 'brawl_modules.rel', wc, inputs / 'main.dol')
        project = 'projects/brawl/recomp.yml'
        # Rev 2 matches doldecomp's 127 input hashes and the checked-in runtime module
        # layout. Retain upstream's rev 1 pin; select this additional known executable
        # only in the staged build configuration. Unknown images still fail validation.
        actual = hashlib.sha256((inputs / 'main_translate.dol').read_bytes()).hexdigest()
        if actual == '27e3afd3e877e64c84cad2de5f0c0b04fe338813aa7aea5d8b19b509b7ab2ac6':
            config = wc / project
            config.write_text(re.sub(r'(?m)^(    sha256:) [a-f0-9]+$',
                                     rf'\1 {actual}', config.read_text(), count=1))
        run(dotnet, translator, 'translate-recursive', '0x8000403C', '--project', project,
            '--outdir', 'generated/functions', '--output-metadata', 'generated/base_translation_output.json',
            '--production-source-bundle', 'generated/base_translation_sources.bin', '--no-function-files',
            '--no-state-free-abi', '--data-in-text', 'projects/brawl/resident_data_ranges.txt',
            '--prune-stale', '--threads', args.jobs, cwd=wc)
        run(dotnet, translator, 'generate-data-init', '--project', project, cwd=wc)
        run(dotnet, translator, 'emit-build-shards', '--project', project,
            '--base-metadata', 'generated/base_translation_output.json', '--base-functions-dir', 'generated/functions',
            '--native-source-dir', 'runtime/src', '--out', 'generated/build_shards', '--base-shards', '600', cwd=wc)
        translated_stamp.write_text(translation_key)
    compilers = f'{shutil.which("clang")}\n{shutil.which("clang++")}'
    compilers_stamp = build / 'native-compilers.txt'
    if compilers_stamp.exists() and compilers_stamp.read_text() != compilers:
        # CMake would reset the cache itself on a compiler change and drop the -D options below with it.
        (build / 'native/CMakeCache.txt').unlink(missing_ok=True)
    compilers_stamp.write_text(compilers)
    run(args.cmake, '-S', wc / 'runtime', '-B', build / 'native', '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_C_COMPILER={shutil.which("clang")}',
        f'-DCMAKE_CXX_COMPILER={shutil.which("clang++")}', f'-DMKW_TRANSLATED_COMPILE_JOBS={args.jobs}', *args.native_arg)
    run(args.cmake, '--build', build / 'native', '--target', 'WiiCompiled', '--parallel', args.jobs)
    # Portable like Play\: settings, saves and logs go to native/UserData, whose config points the game at the disc.
    (build / 'native/portable.txt').write_text('Settings, saves and logs stay in the UserData folder next to this file.\n')
    config = build / 'native/UserData/Config.toml'
    config.parent.mkdir(exist_ok=True)
    text = config.read_text() if config.exists() else ''
    setting = f'dvd_root = {json.dumps(str(game))}'  # the build owns this one setting: -DGAME changes it
    text, found = re.subn(r'(?m)^dvd_root\s*=.*$', lambda _: setting, text)
    if not found:
        text, found = re.subn(r'(?m)^\[paths\]\s*$', lambda _: f'[paths]\n{setting}', text, count=1)
    config.write_text(text if found else f'{text}\n[paths]\n{setting}\n')
    print(f'Built: {build / "native/WiiCompiled"}, playing {game}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, KeyError) as error:
        raise SystemExit(f'Build failed: {error}')

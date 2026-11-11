#!/usr/bin/env python3
"""Native Ubuntu build driver. All mutable state belongs to the build directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SDK = '8.0.425'
SDK_HASHES = {
    'x86_64': ('x64', '934b8060a7190e5909ad1fd0785db542f487b3bbf6cdd14826b02095fdd0d0394298b1634085eff302928fccc33f7c1a7253e9b87df555fc36fce819bcd2e798'),
    'aarch64': ('arm64', '84a4d017d74d7aa842e981679d1b044e6be1f35b9b2b214021e30bc40871d016d29611cca373d8502ad5c890e3ad360ee698f9cf2d7a4d5a9fba102d88ba310f'),
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
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        tar.extractall(destination, filter='data')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    disc, build = args.disc.resolve(strict=True), args.build_dir.resolve()
    if build == ROOT or ROOT.is_relative_to(build):
        parser.error('build directory must not contain the source checkout')
    build.mkdir(parents=True, exist_ok=True)
    # A second build must not overwrite extracted inputs while the first uses them.
    import fcntl
    with (build / 'build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        build_game(args, disc, build)


def build_game(args, disc, build):
    for tool in ('git', 'clang', 'clang++', 'ninja'):
        if not shutil.which(tool):
            raise RuntimeError(f'Missing {tool}. Install the Ubuntu build prerequisites: sudo apt install git clang lld ninja-build build-essential pkg-config')
    downloads = build / 'downloads'
    downloads.mkdir(exist_ok=True)
    arch, checksum = SDK_HASHES[platform.machine()]
    dotnet_dir = build / 'dotnet'
    dotnet = dotnet_dir / 'dotnet'
    if not dotnet.exists():
        archive = downloads / f'dotnet-sdk-{SDK}-{arch}.tar.gz'
        download(f'https://builds.dotnet.microsoft.com/dotnet/Sdk/{SDK}/dotnet-sdk-{SDK}-linux-{arch}.tar.gz', archive, checksum, 'sha512')
        unpack(archive, dotnet_dir)
    os.environ.update(DOTNET_ROOT=str(dotnet_dir), DOTNET_CLI_TELEMETRY_OPTOUT='1',
                      DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_NOLOGO='1')
    # Stage only source inputs. Upstream's generator writes beside runtime/ and projects/.
    # Recreate on source changes so deleted source files cannot survive in a cached build.
    source_files = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-co', '--exclude-standard', '-z']).split(b'\0')
    source_files = [raw for raw in source_files if raw and
                    not (ROOT / os.fsdecode(raw)).is_relative_to(build) and
                    (raw.startswith((b'wiicompiled/', b'tools/disc-extract/')) or
                     raw in (b'launcher/WiiDisc.cs', b'tools/build_ubuntu.py'))]
    source_hash = hashlib.sha256()
    for raw in sorted(filter(None, source_files)):
        path = ROOT / os.fsdecode(raw)
        if path.is_file():
            source_hash.update(raw)
            source_hash.update(path.read_bytes())
    fingerprint = source_hash.hexdigest()
    work = build / 'source'
    source_stamp = build / 'source.sha256'
    if not source_stamp.exists() or source_stamp.read_text() != fingerprint:
        if work.exists():
            shutil.rmtree(work)
        work.mkdir()
        for raw in filter(None, source_files):
            source = ROOT / os.fsdecode(raw)
            if source.is_file():
                target = work / os.fsdecode(raw)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        source_stamp.write_text(fingerprint)
        (build / 'translated.json').unlink(missing_ok=True)
    wc = work / 'wiicompiled'
    for project in ('tools/disc-extract/DiscExtract.csproj', 'wiicompiled/translator/src/Translator.Cli/Translator.Cli.csproj'):
        run(dotnet, 'build', work / project, '-c', 'Release', '--nologo')
    extractor = work / 'tools/disc-extract/bin/Release/net8.0/DiscExtract.dll'
    translator = wc / 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
    images = [disc]
    if disc.suffix.lower() == '.wbfs':
        i = 1
        while disc.with_suffix(f'.wbf{i}').exists():
            images.append(disc.with_suffix(f'.wbf{i}'))
            i += 1
    identity = [(str(p), p.stat().st_size, p.stat().st_mtime_ns) for p in images]
    extraction_stamp = build / 'extracted.json'
    extraction_key = json.dumps(identity)
    data = build / 'disc'
    if not extraction_stamp.exists() or extraction_stamp.read_text() != extraction_key:
        temporary = build / 'disc.partial'
        if temporary.exists():
            shutil.rmtree(temporary)
        run(dotnet, extractor, disc, temporary)
        if data.exists():
            shutil.rmtree(data)
        temporary.rename(data)
        extraction_stamp.write_text(extraction_key)
        (build / 'translated.json').unlink(missing_ok=True)
    decomp = build / f'brawl-{DECOMP}'
    if not decomp.exists():
        archive = downloads / f'brawl-{DECOMP}.tar.gz'
        download(f'https://codeload.github.com/doldecomp/brawl/tar.gz/{DECOMP}', archive,
                 '1ae548ca5d22859e9475d3af47bc2b63a33a63c16d9e0ad61d2579b70b9806b8')
        unpack(archive, build)
    translated_stamp = build / 'translated.json'
    if not translated_stamp.exists():
        game = work / 'game'
        game.mkdir(exist_ok=True)
        shutil.copy2(data / 'sys/main.dol', game / 'main.dol')
        run('python3', wc / 'projects/brawl/tools/build_modules.py', data / 'files/module',
            decomp / 'config/RSBE01_02', game / 'brawl_modules.rel', wc, game / 'main.dol')
        project = 'projects/brawl/recomp.yml'
        # Rev 2 matches doldecomp's 127 input hashes and the checked-in runtime module
        # layout. Retain upstream's rev 1 pin; select this additional known executable
        # only in the staged build configuration. Unknown images still fail validation.
        actual = hashlib.sha256((game / 'main_translate.dol').read_bytes()).hexdigest()
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
        translated_stamp.write_text(extraction_key)
    run(args.cmake, '-S', wc / 'runtime', '-B', build / 'native', '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_C_COMPILER={shutil.which("clang")}',
        f'-DCMAKE_CXX_COMPILER={shutil.which("clang++")}', f'-DMKW_TRANSLATED_COMPILE_JOBS={args.jobs}')
    run(args.cmake, '--build', build / 'native', '--target', 'WiiCompiled', '--parallel', args.jobs)
    print(f'Built: {build / "native/WiiCompiled"}\nExtracted game data: {data}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, KeyError) as error:
        raise SystemExit(f'Build failed: {error}')

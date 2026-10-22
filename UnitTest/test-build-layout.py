"""Verify evaluated MSBuild paths, deployable inputs, and a clean source tree."""
import argparse
import ctypes
import json
from pathlib import Path
import struct
import subprocess


def machine(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<H', data, pe + 4)[0]


def compile_resource(path):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p]
    kernel.FindResourceW.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.SizeofResource.restype = ctypes.c_uint32
    kernel.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.LoadResource.restype = ctypes.c_void_p
    kernel.LockResource.argtypes = [ctypes.c_void_p]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    module = kernel.LoadLibraryExW(str(path), None, 2)  # Data-only mapping; never execute player code.
    assert module, ctypes.get_last_error()
    try:
        resource = kernel.FindResourceW(module, 540, 'TEXT')
        assert resource, 'compile time resource missing'
        size = kernel.SizeofResource(module, resource)
        memory = kernel.LockResource(kernel.LoadResource(module, resource))
        return ctypes.string_at(memory, size)
    finally:
        kernel.FreeLibrary(module)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--msbuild', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = root / 'build'
    projects = [root / 'MusicPlayer2/MusicPlayer2.vcxproj',
                root / 'scintilla/win32/SciLexer.vcxproj',
                root / 'scintilla/win32/Scintilla.vcxproj',
                root / 'UnitTest/UnitTest.vcxproj']
    cases = 0
    for project in projects:
        configurations = ('Debug',) if project.stem == 'UnitTest' else ('Debug', 'Release')
        for platform in ('Win32', 'x64'):
            for configuration in configurations:
                output = subprocess.check_output([
                    str(args.msbuild), str(project), '-nologo',
                    '-p:Platform=' + platform, '-p:Configuration=' + configuration,
                    '-getProperty:OutDir,IntDir',
                ], encoding='utf-8')
                properties = json.loads(output)['Properties']
                expected = build if (platform, configuration) == ('x64', 'Release') else build / 'variants' / platform / configuration
                if project.stem == 'UnitTest':
                    expected = root / 'test-results/native' / platform / configuration
                assert Path(properties['OutDir']) == expected, properties
                intermediate_root = root / 'test-results' if project.stem == 'UnitTest' else build
                assert Path(properties['IntDir']) == intermediate_root / 'obj' / project.stem / platform / configuration, properties
                cases += 1
    for name in ('MusicPlayer2.exe', 'bass.dll', 'bass_fx.dll', 'tag.dll', 'SciLexer.dll'):
        assert machine(build / name) == 0x8664, name
    assert (build / 'language/Simplified_Chinese.ini').is_file()
    assert (build / 'skins').is_dir()
    stamp = build / 'obj/MusicPlayer2/x64/Release/compile_time.txt'
    embedded = compile_resource(build / 'MusicPlayer2.exe')
    assert embedded.strip() == stamp.read_bytes().strip(), 'resource did not use intermediate directory'
    commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD']).strip()
    assert embedded.splitlines()[1].strip() == commit, 'executable does not match current source commit'
    for dependency in (root / 'third_party/runtime').rglob('*.dll'):
        expected = 0x8664 if 'x64' in dependency.parts else 0x14C
        assert machine(dependency) == expected, dependency
    for old in ('Debug', 'Release', 'x64', 'x86', 'MusicPlayer2/x64',
                'MusicPlayer2/MusicPlayer2', 'scintilla/win32/x64',
                'MusicPlayer2/compile_time.txt'):
        assert not (root / old).exists(), old
    extensions = {'.obj', '.pdb', '.pch', '.ilk', '.tlog', '.res', '.bsc', '.sbr'}
    for folder in (root / 'MusicPlayer2', root / 'scintilla', root / 'UnitTest'):
        for path in folder.rglob('*'):
            assert path.suffix.lower() not in extensions, path
    assert not subprocess.check_output(['git', '-C', str(root), 'ls-files', 'build'])
    assert not (build / 'tests').exists(), 'test output belongs in test-results/'
    assert not (build / 'obj/UnitTest').exists(), 'test intermediate files belong in test-results/'
    print(f'Build layout passed: {cases} project configurations, x64 executable/DLLs, embedded Git commit, resources, no generated source files.')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Stage an independent Windows build job for an installed iiVirtualizer VM."""
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import uuid

from prepare_build_payload import snapshot
from prepare_os_guests import generated_path


DEFAULT_PATH = ('C:\\iiv\\tools;C:\\iiv\\tools\\clang\\bin;C:\\iiv\\tools\\cmake\\bin;'
                'C:\\Windows\\system32;C:\\Windows')


def relative_name(value):
    """Reject absolute, platform-specific and parent-traversing job paths."""
    if not isinstance(value, str) or not value or '\\' in value or ':' in value:
        raise ValueError('Expected a relative forward-slash path')
    parts = value.split('/')
    if any(part in ('', '.', '..') for part in parts) or PurePosixPath(value).is_absolute():
        raise ValueError('Expected a relative forward-slash path')
    return value


def checked_recipe(recipe):
    """Validate the public job recipe before creating any output files."""
    if not isinstance(recipe, dict) or set(recipe) != {'steps', 'artifacts'}:
        raise ValueError('A recipe contains only steps and artifacts')
    steps, artifacts = recipe['steps'], recipe['artifacts']
    if not isinstance(steps, list) or not 1 <= len(steps) <= 32:
        raise ValueError('A recipe requires 1 to 32 steps')
    if not isinstance(artifacts, list) or len(artifacts) > 64:
        raise ValueError('A recipe permits at most 64 artifacts')
    names = [relative_name(name) for name in artifacts]
    if len(names) != len(set(names)):
        raise ValueError('Artifact names must be unique')
    prepared = []
    for step in steps:
        if not isinstance(step, dict) or not {'argv', 'timeout_ms'} <= set(step) or set(step) - {'argv', 'timeout_ms', 'cwd', 'path'}:
            raise ValueError('Invalid build step fields')
        args, timeout = step['argv'], step['timeout_ms']
        if (not isinstance(args, list) or not 1 <= len(args) <= 256 or
                any(not isinstance(arg, str) or '\0' in arg for arg in args) or not args[0]):
            raise ValueError('A build step needs a nonempty argv array')
        if isinstance(timeout, bool) or not isinstance(timeout, int) or not 1 <= timeout <= 3600000:
            raise ValueError('A build step deadline must be 1..3600000 milliseconds')
        item = dict(step)
        if 'cwd' in item:
            relative_name(item['cwd'])
        if 'path' in item and (not isinstance(item['path'], str) or '\0' in item['path']):
            raise ValueError('The step PATH must be text')
        item.setdefault('path', DEFAULT_PATH)
        prepared.append(item)
    return prepared, names


def stage_job(source, recipe, output):
    """Snapshot a project and create a uniquely identified, offline guest job."""
    steps, artifacts = checked_recipe(recipe)
    source = Path(source).resolve(strict=True)
    output = generated_path(output)
    if not source.is_dir() or source == output or source in output.parents:
        raise ValueError('Source must be a directory outside the job output')
    if output.exists():
        raise ValueError('Choose a fresh job output directory')
    identity = uuid.uuid4().hex
    workspace = 'C:/iiv/jobs/' + identity
    job = {'job': identity, 'workspace': workspace, 'steps': steps, 'artifacts': artifacts}
    encoded = (json.dumps(job, indent=2) + '\n').encode('utf-8')
    if len(encoded) > 65536:
        raise ValueError('Job manifest exceeds the worker limit')
    output.mkdir(parents=True, mode=0o700)
    try:
        media = output / 'media' / 'IIVJOB'
        media.mkdir(parents=True)
        inventory = snapshot(source, media / 'source')
        if not inventory:
            raise ValueError('The source snapshot contains no files')
        (media / 'job.json').write_bytes(encoded)
        (output / 'job.json').write_bytes(encoded)
        (output / 'source-manifest.json').write_text(json.dumps(inventory, indent=2) + '\n')
        # This script is generated from a hexadecimal identity, not from a
        # recipe command. Commands remain argv arrays consumed by the worker.
        target = 'C:\\iiv\\jobs\\' + identity
        lines = [
            '@echo off',
            'setlocal',
            f'if exist "{target}" exit /b 2',
            f'mkdir "{target}\\source"',
            'if errorlevel 1 exit /b 3',
            f'xcopy /E /I /Y /Q "%~dp0source\\*" "{target}\\source" >nul',
            'if errorlevel 1 exit /b 4',
            f'copy /Y "%~dp0job.json" "{target}\\job.json" >nul',
            'if errorlevel 1 exit /b 5',
            f'C:\\iiv\\agent\\iiv-build-agent.exe "{target}\\job.json" \\\\.\\COM1 >"{target}\\agent-start.log" 2>&1',
            'exit /b %errorlevel%',
        ]
        (media / 'stage.cmd').write_bytes(('\r\n'.join(lines) + '\r\n').encode('ascii'))
    except BaseException:
        shutil.rmtree(output)
        raise
    return {'job': identity, 'workspace': workspace, 'output': str(output),
            'job_manifest': str(output / 'job.json'), 'files': len(inventory)}


def make_iso(output):
    """Create a read-only job medium using the host's native ISO tool."""
    output = Path(output)
    image = output / 'windows-job.iso'
    if sys.platform == 'darwin':
        command = ['hdiutil', 'makehybrid', '-iso', '-joliet', '-iso-volume-name', 'IIVJOB',
                   '-joliet-volume-name', 'IIVJOB', '-o', str(image), str(output / 'media')]
    else:
        maker = shutil.which('genisoimage')
        if not maker:
            raise RuntimeError('genisoimage is required to make a job ISO on this host')
        command = [maker, '-quiet', '-R', '-J', '-iso-level', '3', '-V', 'IIVJOB',
                   '-o', str(image), str(output / 'media')]
    subprocess.run(command, check=True)
    return image


def main():
    """Create a job directory and ISO without modifying the source project."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--recipe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = stage_job(args.source, json.loads(args.recipe.read_text()), args.output)
    result['iso'] = str(make_iso(result['output']))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

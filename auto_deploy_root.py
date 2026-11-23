#!/usr/bin/env python3
"""Monitor GitHub Actions root build and deploy to Galaxy S10e /dev/block/sda34."""
import json
import os
import subprocess
import sys
import time
import urllib.request
import zipfile

RUN_ID = 45268244999
TOKEN = os.environ.get('GH_TOKEN', '')
REPO = 'michas-xyz/ios-beyond0lte'
ADB = r'C:\Users\miket\AppData\Local\Android\Sdk\platform-tools\adb.exe'
TARGET_PARTITION = '/dev/block/sda34'


def check_run():
    url = f'https://api.github.com/repos/{REPO}/actions/runs/{RUN_ID}'
    req = urllib.request.Request(url, headers={
        'Authorization': f'token {TOKEN}',
        'Accept': 'application/vnd.github.v3+json'
    })
    try:
        with urllib.request.urlopen(req) as resp:
            data = json.load(resp)
            return data.get('status'), data.get('conclusion')
    except Exception as e:
        print(f'Error querying run: {e}')
        return None, None


def get_jobs():
    url = f'https://api.github.com/repos/{REPO}/actions/runs/{RUN_ID}/jobs'
    req = urllib.request.Request(url, headers={
        'Authorization': f'token {TOKEN}',
        'Accept': 'application/vnd.github.v3+json'
    })
    try:
        with urllib.request.urlopen(req) as resp:
            data = json.load(resp)
            for j in data.get('jobs', []):
                for s in j.get('steps', []):
                    if s.get('status') == 'in_progress':
                        return f"{j.get('name')}: {s.get('name')} (in_progress)"
            return data.get('jobs', [{}])[0].get('status', 'unknown')
    except Exception:
        return 'querying...'


def main():
    print(f'Monitoring GitHub Actions Run {RUN_ID} (Build S10e Full Real iOS Root)...')
    while True:
        status, conclusion = check_run()
        job_info = get_jobs()
        now = time.strftime('%H:%M:%S')
        print(f'[{now}] Status: {status}, Conclusion: {conclusion} | Current: {job_info}')

        if status == 'completed':
            if conclusion == 'success':
                print('Build succeeded! Proceeding to download and flash...')
                break
            else:
                print(f'Build finished with conclusion: {conclusion}')
                sys.exit(1)

        time.sleep(15)


if __name__ == '__main__':
    main()

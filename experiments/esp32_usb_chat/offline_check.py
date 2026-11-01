"""Explicit normal device check with a deterministic model fixture; no network/API key."""
import json
import argparse
from pathlib import Path
import sys
import time
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'desktop'))
from bridge import Bridge

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--speaker', action='store_true')
    parser.add_argument('--batch', action='store_true', help='Group three inspections, then tone/speech/screen in one model reply')
    args=parser.parse_args()
    if args.batch and not args.speaker:
        parser.error('--batch requires --speaker')
    events = []
    def emit(kind, value):
        events.append({'type':kind,'value':value})
        print(json.dumps(events[-1],ensure_ascii=False),flush=True)
    config = {'port':'/dev/cu.usbmodem5D013686941','url':'https://api.deepseek.com',
              'model':'deepseek-flash','key':''}
    bridge = Bridge(config,emit,str(ROOT/'build/AyniUSBChat'),offline=True,speaker_check=args.speaker,batch_check=args.batch)
    bridge.connect()
    deadline = time.monotonic()+120
    submitted = False
    success = False
    while time.monotonic()<deadline:
        bridge.poll()
        if bridge.ready and not submitted:
            bridge.user('请检查提示音、中文朗读和屏幕能力。' if args.speaker else '请调用屏幕能力显示中文检查结果。')
            submitted = True
        if any(e['type']=='answer' for e in events):
            success = any(e['type']=='status' and e['value']=='screen capability completed' for e in events)
            break
        if any(e['type']=='error' for e in events):
            break
    if success and len(bridge.pages)>1:
        bridge.show_page(len(bridge.pages)-1)
        deadline=time.monotonic()+15
        while bridge.display_waiting is not None and time.monotonic()<deadline:
            bridge.poll()
        success = bridge.display_waiting is None
        if success:
            bridge.show_page(0)
            deadline=time.monotonic()+15
            while bridge.display_waiting is not None and time.monotonic()<deadline:
                bridge.poll()
            success = bridge.display_waiting is None
    if args.speaker:
        success = success and sum(e['type']=='status' and e['value']=='speaker playback completed' for e in events)==2
        success = success and sum(e['type']=='speaker_verified' for e in events)==2
    bridge.close(timeout=10)
    output=ROOT/'artifacts'/('batch-speaker-device-events.json' if args.batch else 'speaker-device-events.json' if args.speaker else 'offline-device-events.json')
    output.write_text(json.dumps({'passed':success,'normal_stop':bridge.closed,'events':events},ensure_ascii=False,indent=2))
    if not success or not bridge.closed:
        raise SystemExit('Normal device check did not complete; inspect events, do not claim success')
    print('Normal on-device capability chain and joined shutdown passed; no real model call.')

if __name__=='__main__': main()

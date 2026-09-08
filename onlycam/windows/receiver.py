"""Receive OnlyCam MJPEG from iPhone and optionally publish a Windows virtual webcam."""

from __future__ import annotations

import argparse
import sys
import time

import cv2
import numpy as np
import requests


def frames(url: str):
    with requests.get(url, stream=True, timeout=10) as response:
        response.raise_for_status()
        buffer = b""
        for chunk in response.iter_content(chunk_size=16384):
            if not chunk:
                continue
            buffer += chunk
            while True:
                start = buffer.find(b"\xff\xd8")
                end = buffer.find(b"\xff\xd9")
                if start == -1 or end == -1 or end < start:
                    break
                jpeg = buffer[start : end + 2]
                buffer = buffer[end + 2 :]
                image = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
                if image is not None:
                    yield image


def main() -> int:
    parser = argparse.ArgumentParser(description="OnlyCam Windows receiver")
    parser.add_argument("--url", default="http://192.168.0.10:8080/stream", help="iPhone stream URL")
    parser.add_argument("--virtual-cam", action="store_true", help="Send frames to OBS virtual camera")
    parser.add_argument("--fps", type=int, default=30)
    args = parser.parse_args()

    print(f"Connecting to {args.url}")
    print("On iPhone: open OnlyCam, allow Camera + Local Network, keep the app on screen.")
    print("Press Q in the preview window to quit.")

    cam = None
    try:
        for frame in frames(args.url):
            if args.virtual_cam and cam is None:
                try:
                    import pyvirtualcam

                    h, w = frame.shape[:2]
                    cam = pyvirtualcam.Camera(width=w, height=h, fps=args.fps)
                    print(f"Virtual camera started: {cam.device}")
                except Exception as exc:
                    print("Virtual camera failed. Install OBS Studio and start Virtual Camera once.")
                    print(exc)
                    args.virtual_cam = False

            cv2.imshow("OnlyCam", frame)
            if cam is not None:
                cam.send(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
                cam.sleep_until_next_frame()

            if cv2.waitKey(1) & 0xFF in (ord("q"), ord("Q")):
                break
    except requests.RequestException as exc:
        print("Could not connect. Same Wi-Fi? App open? URL correct?")
        print(exc)
        time.sleep(2)
        return 1
    finally:
        if cam is not None:
            cam.close()
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    sys.exit(main())

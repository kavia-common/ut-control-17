#!/usr/bin/env python3
# /*
#  * If not stated otherwise in this file or this component's LICENSE file the
#  * following copyright and licenses apply:
#  *
#  * Copyright 2026 RDK Management
#  *
#  * Licensed under the Apache License, Version 2.0 (the "License");
#  * you may not use this file except in compliance with the License.
#  * You may obtain a copy of the License at
#  *
#  * http://www.apache.org/licenses/LICENSE-2.0
#  *
#  * Unless required by applicable law or agreed to in writing, software
#  * distributed under the License is distributed on an "AS IS" BASIS,
#  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  * See the License for the specific language governing permissions and
#  * limitations under the License.
#  */

"""
Send a control-plane YAML message to a UT Control Plane WebSocket server.

This helper is intended for Virtual-CEC environments where the HDMI-CEC L3 harness
fails with "Failed to assign DUT address" because the virtual TV device (VTV)
has logical address -1 instead of 0.

Usage:
  ./seed-hdmicec-logical-address.py --uri ws://<dut-ip>:8080 --yaml hdmicec-add-logical-address-0.yaml
"""

import argparse
import asyncio
import os
from typing import Any, Dict

import websockets
import yaml


def _read_yaml(file_path: str) -> Dict[str, Any]:
    with open(file_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    if not isinstance(data, dict):
        raise ValueError(f"YAML root must be a mapping/dict, got {type(data).__name__}")
    return data


async def _send_yaml(uri: str, yaml_data: Dict[str, Any]) -> None:
    async with websockets.connect(uri) as websocket:
        await websocket.send(yaml.dump(yaml_data))
        # Many UT control-plane servers do not send a response; keep this best-effort.
        try:
            response = await asyncio.wait_for(websocket.recv(), timeout=1.0)
            print(f"Received: {response}")
        except Exception:
            pass
        print("YAML seed message sent successfully")


def main() -> None:
    parser = argparse.ArgumentParser(description="Seed Virtual HDMI-CEC logical addresses via ut-control WebSocket.")
    parser.add_argument("--uri", default="ws://localhost:8080", help="WebSocket URI (default: ws://localhost:8080)")
    parser.add_argument(
        "--yaml",
        dest="yaml_path",
        default="hdmicec-add-logical-address-0.yaml",
        help="Path to YAML file to send (default: hdmicec-add-logical-address-0.yaml)",
    )
    args = parser.parse_args()

    yaml_path = os.path.abspath(args.yaml_path)
    yaml_data = _read_yaml(yaml_path)
    asyncio.get_event_loop().run_until_complete(_send_yaml(args.uri, yaml_data))


if __name__ == "__main__":
    main()

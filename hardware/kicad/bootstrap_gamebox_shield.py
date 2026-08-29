#!/usr/bin/env python3
"""根据 esp32s3-gamebox 固件引脚分配，生成 KiCad 10 载板工程。

用法（需已安装 mcp-server-kicad，见项目 .cursor/mcp.json）：
  uv run --with mcp-server-kicad python hardware/kicad/bootstrap_gamebox_shield.py

引脚来源：main/display.h、main/input_gamepad.h、main/audio_output.c、docs/hardware.md
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# 让 mcp_server_kicad 的 place_component 能找到 KiCad 自带符号库
if sys.platform == "darwin":
    for app in (
        Path.home() / "Applications/KiCad.app",
        Path("/Applications/KiCad.app"),
    ):
        sym = app / "Contents/SharedSupport/symbols"
        if sym.is_dir():
            os.environ.setdefault("KICAD_SYMBOL_DIR", str(sym))
            break

from mcp_server_kicad.pcb import add_pcb_line, move_footprint, update_pcb_from_schematic
from mcp_server_kicad.project import create_project
from mcp_server_kicad.schematic import (
    add_power_symbol,
    add_text,
    no_connect_pin,
    place_component,
    set_component_property,
    wire_pins_to_net,
)

ROOT = Path(__file__).resolve().parent / "gamebox-shield"
NAME = "gamebox-shield"
SCH = str(ROOT / f"{NAME}.kicad_sch")
PCB = str(ROOT / f"{NAME}.kicad_pcb")
PRO = str(ROOT / f"{NAME}.kicad_pro")

# PCB 布局常量（DevKitC 两排中心距 28 mm，见 docs/hardware.md §1）
DEVKIT_ROW_GAP_MM = 28.0
BOARD_W_MM = 86.0
BOARD_H_MM = 58.0
BOARD_MARGIN_MM = 4.0

# Conn_01x22：pin 1 在符号顶端，pin 22 在底端；与 hardware.md §2 左→右顺序一致。
# 下排 J1：pin N = 下排从左数第 N 个焊盘。
# 上排 J2：pin N = 上排从左数第 N 个焊盘。
DEVKIT_J1_NETS: dict[str, str] = {
    "1": "+3V3",
    "2": "+3V3",
    "3": "NC",  # RST（EN），非 GPIO
    "4": "GPIO4_BCLK",
    "5": "GPIO5_LRC",
    "6": "GPIO6_DIN",
    "7": "NC",  # GPIO7 空闲（START 已改到 GPIO21）
    "8": "GPIO15_BTN_X",
    "9": "GPIO16_BTN_A",
    "10": "GPIO17_BTN_B",
    "11": "GPIO18_BTN_Y",
    "12": "NC",  # GPIO8 空闲（SELECT 已改到 GPIO47）
    "13": "NC",  # GPIO3 strapping
    "14": "NC",  # GPIO46 strapping
    "15": "GPIO9_BL",
    "16": "GPIO10_CS",
    "17": "GPIO11_MOSI",
    "18": "GPIO12_SCK",
    "19": "GPIO13_RST",
    "20": "GPIO14_DC",
    "21": "NC",  # 5Vin
    "22": "GND",
}

DEVKIT_J2_NETS: dict[str, str] = {
    "1": "GND",
    "2": "NC",  # TX / GPIO43
    "3": "NC",  # RX / GPIO44
    "4": "GPIO1_JOY_X",
    "5": "GPIO2_JOY_Y",
    "6": "NC",
    "7": "NC",
    "8": "NC",
    "9": "NC",
    "10": "NC",
    "11": "NC",
    "12": "NC",
    "13": "NC",
    "14": "NC",
    "15": "NC",
    "16": "NC",  # GPIO48 板载 RGB，载板不引
    "17": "GPIO47_SELECT",
    "18": "GPIO21_START",
    "19": "NC",  # GPIO20 USB
    "20": "NC",  # GPIO19 USB
    "21": "GND",
    "22": "GND",
}


def _wire(net: str, pins: list[dict[str, str]]) -> None:
    wire_pins_to_net(pins, net, schematic_path=SCH)


def _pin(ref: str, pin: str) -> dict[str, str]:
    return {"reference": ref, "pin": pin}


def _nc(ref: str, pin: str) -> None:
    no_connect_pin(ref, pin, schematic_path=SCH)


def _place_conn(
    ref: str,
    lib_id: str,
    value: str,
    x: float,
    y: float,
    rot: int = 0,
    footprint: str = "",
) -> None:
    place_component(
        lib_id=lib_id,
        reference=ref,
        value=value,
        x=x,
        y=y,
        rotation=rot,
        schematic_path=SCH,
        project_path=PRO,
    )
    if footprint:
        set_component_property(ref, "Footprint", footprint, schematic_path=SCH)


def _wire_devkit_header(ref: str, pin_nets: dict[str, str]) -> None:
    """按 pin→网络表连线；同一网络的多 pin 合并为一次 wire_pins_to_net。"""
    by_net: dict[str, list[dict[str, str]]] = {}
    for pin, net in pin_nets.items():
        if net == "NC":
            _nc(ref, pin)
            continue
        by_net.setdefault(net, []).append(_pin(ref, pin))
    for net, pins in by_net.items():
        _wire(net, pins)


def build_schematic() -> None:
    if ROOT.exists():
        import shutil

        shutil.rmtree(ROOT)
    create_project(str(ROOT), NAME)

    # --- 标题与说明 ---
    add_text(
        "ESP32-S3 Gamebox Shield — DevKitC-1 + ST7789 + JoyStick Shield + MAX98357",
        30,
        20,
        schematic_path=SCH,
    )
    add_text(
        "固件引脚见仓库 docs/hardware.md；Shield 摇杆拨到 3V3",
        30,
        26,
        schematic_path=SCH,
    )

    # 电源
    add_power_symbol("power:+3V3", "#PWR01", 40, 45, schematic_path=SCH)
    add_power_symbol("power:GND", "#PWR02", 40, 55, schematic_path=SCH)

    # DevKitC-1 双排母座 — 水平并排，避免竖直叠放时 J1 底脚与 J2 顶脚 Y 坐标重叠而错网
    _place_conn(
        "J1",
        "Connector_Generic:Conn_01x22",
        "DevKitC 下排",
        48,
        95,
        footprint="Connector_PinSocket_2.54mm:PinSocket_1x22_P2.54mm_Vertical",
    )
    _place_conn(
        "J2",
        "Connector_Generic:Conn_01x22",
        "DevKitC 上排",
        98,
        95,
        footprint="Connector_PinSocket_2.54mm:PinSocket_1x22_P2.54mm_Vertical",
    )

    # ST7789 8pin（GND VCC SCK MOSI RST DC CS BLK）
    _place_conn(
        "J3",
        "Connector_Generic:Conn_01x08",
        "ST7789 SPI",
        155,
        70,
        footprint="Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical",
    )

    # JoyStick Shield 10 根杜邦线（按 input_gamepad.h 顺序做成 1x10 座）
    _place_conn(
        "J4",
        "Connector_Generic:Conn_01x10",
        "JoyStick Shield",
        155,
        100,
        footprint="Connector_PinSocket_2.54mm:PinSocket_1x10_P2.54mm_Vertical",
    )

    # MAX98357 I2S 模块（常见 6pin：VIN GND BCLK LRC DIN + SD 悬空）
    _place_conn(
        "J5",
        "Connector_Generic:Conn_01x06",
        "MAX98357",
        155,
        140,
        footprint="Connector_PinSocket_2.54mm:PinSocket_1x06_P2.54mm_Vertical",
    )

    # 喇叭（桥接输出，两端均不接 GND；焊到模块 SPK+/SPK- 焊盘）
    _place_conn(
        "J6",
        "Connector_Generic:Conn_01x02",
        "Speaker",
        155,
        170,
        footprint="Connector_PinSocket_2.54mm:PinSocket_1x02_P2.54mm_Vertical",
    )

    # 接线表（文字注释，对应 J1/J2 丝印顺序 — hardware.md §2）
    add_text(
        "J1 下排(左→右): 3V3 3V3 RST 4 5 6 7 15 16 17 18 8 3 46 9 10 11 12 13 14 5Vin GND",
        30,
        175,
        schematic_path=SCH,
    )
    add_text(
        "J2 上排(左→右): GND TX RX 1 2 42 41 40 39 38 37 36 35 0 45 48 47 21 20 19 GND GND",
        30,
        181,
        schematic_path=SCH,
    )
    add_text(
        "J3 屏: GND 3V3 SCK(GPIO12) MOSI(GPIO11) RST(GPIO13) DC(GPIO14) CS(GPIO10) BLK(GPIO9)",
        30,
        187,
        schematic_path=SCH,
    )
    add_text(
        "J4 Shield 1x10: 1G 23V3 3X 4Y 5A 6B 7C 8D 9F(SEL) 10E(START)",
        30,
        193,
        schematic_path=SCH,
    )
    add_text(
        "J5 MAX98357: VIN→3V3  GND  BCLK→GPIO4  LRC→GPIO5  DIN→GPIO6  SD 悬空",
        30,
        199,
        schematic_path=SCH,
    )
    add_text(
        "J6 → MAX98357 模块 SPK+/SPK- 焊盘（6pin 座不含喇叭脚）",
        30,
        205,
        schematic_path=SCH,
    )

    # --- DevKit 双排针（权威映射表驱动，避免 pin 号手抄漂移）---
    _wire_devkit_header("J1", DEVKIT_J1_NETS)
    _wire_devkit_header("J2", DEVKIT_J2_NETS)

    # --- 外设座子 ↔ 已布好的 DevKit 网络 ---
    _wire("+3V3", [_pin("J3", "2"), _pin("J4", "2"), _pin("J5", "1")])
    _wire("GND", [_pin("J3", "1"), _pin("J4", "1"), _pin("J5", "2")])
    _wire("GPIO1_JOY_X", [_pin("J4", "3")])
    _wire("GPIO2_JOY_Y", [_pin("J4", "4")])
    _wire("GPIO21_START", [_pin("J4", "10")])
    _wire("GPIO47_SELECT", [_pin("J4", "9")])
    _wire("GPIO15_BTN_X", [_pin("J4", "5")])
    _wire("GPIO16_BTN_A", [_pin("J4", "6")])
    _wire("GPIO17_BTN_B", [_pin("J4", "7")])
    _wire("GPIO18_BTN_Y", [_pin("J4", "8")])
    _wire("GPIO9_BL", [_pin("J3", "8")])
    _wire("GPIO10_CS", [_pin("J3", "7")])
    _wire("GPIO11_MOSI", [_pin("J3", "4")])
    _wire("GPIO12_SCK", [_pin("J3", "3")])
    _wire("GPIO13_RST", [_pin("J3", "5")])
    _wire("GPIO14_DC", [_pin("J3", "6")])
    _wire("GPIO4_BCLK", [_pin("J5", "3")])
    _wire("GPIO5_LRC", [_pin("J5", "4")])
    _wire("GPIO6_DIN", [_pin("J5", "5")])
    _wire("SPK+", [_pin("J6", "1")])
    _wire("SPK-", [_pin("J6", "2")])
    _nc("J5", "6")  # SD：模块常带上拉，载板不控关断


def _board_outline(x0: float, y0: float, w: float, h: float) -> None:
    """矩形板框（Edge.Cuts）。"""
    x1, y1 = x0 + w, y0 + h
    for xa, ya, xb, yb in (
        (x0, y0, x1, y0),
        (x1, y0, x1, y1),
        (x1, y1, x0, y1),
        (x0, y1, x0, y0),
    ):
        add_pcb_line(xa, ya, xb, yb, layer="Edge.Cuts", pcb_path=PCB)


def build_pcb() -> None:
    result = update_pcb_from_schematic(
        schematic_path=SCH, pcb_path=PCB, project_path=PRO
    )
    print("update_pcb_from_schematic:", result)

    # DevKit 两排沿 Y 方向排 pin、沿 X 相距 28 mm（与实物一致，不可上下叠放）
    row_y = BOARD_H_MM / 2
    j1_x = BOARD_MARGIN_MM + 8
    j2_x = j1_x + DEVKIT_ROW_GAP_MM
    peri_x = j2_x + 22  # 外设座在 DevKit 右侧

    placements = [
        ("J1", j1_x, row_y, 0),
        ("J2", j2_x, row_y, 0),
        ("J3", peri_x, 12, 90),
        ("J4", peri_x, 26, 90),
        ("J5", peri_x, 38, 90),
        ("J6", peri_x, 50, 90),
    ]
    for ref, x, y, rot in placements:
        try:
            move_footprint(ref, x, y, rotation=rot, pcb_path=PCB)
        except Exception as exc:  # noqa: BLE001
            print(f"move_footprint {ref}: {exc}")

    _board_outline(0, 0, BOARD_W_MM, BOARD_H_MM)


def main() -> None:
    print("生成目录:", ROOT)
    build_schematic()
    print("原理图 OK:", SCH)
    build_pcb()
    print("PCB OK:", PCB)
    print("用 KiCad 10 打开:", PRO)


if __name__ == "__main__":
    main()

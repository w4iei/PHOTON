#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# PHOTON Layout Assistant for KiCad 9
# + Silkscreen label generator from each VCNT center
#
# How to use:
# 1) Run the plugin and enter the desired sensor pitch (mm).
#    It places all VCNT sensors across banks and places their passives.
# 2) Resize the board to the correct width, adjust VCC and GND pours to be the correct width, and place the mouse-bites (if desired) so that the last sensor is cut-off able.
# 3) Fully route/layout bank 1, then use the Replicate Layout plugin to
#    copy the routing/placement to the remaining banks:
#    https://github.com/MitjaNemec/ReplicateLayout
# 4) The last bank will require manual routing, and some middle banks may
#    need manual edits where the MCU is.
#
# Plugin: VcntSensorArrayPlugin
#   - Dialog asks ONLY for sensor pitch (mm)
#   - Finds all VCNT2025X01 footprints (VALUE containing 'VCNT2025')
#   - For each VCNT, resolves its associated R/Q parts by nets:
#       * R_pin3 : resistor on VCNT pin3 net
#       * R_pin2 : resistor on VCNT pin2 net
#       * Q      : transistor with pad3 on VCNT pin4 net
#       * R_q1   : resistor sharing net with Q pad1
#   - Places VCNT + R + Q using fixed coordinates for bank 1,
#     shifted in X by index * pitch for each subsequent bank.
#   - Routes all specified VCNT traces (0.15mm) and vias (0.3/0.6mm).
#   - Adds 0-index numeric labels 2.5 mm below each VCNT center.
#   - Places ADCs (TLA2518) + Cx01..Cx07 using fixed offsets and pitch*4 spacing.
#   - Hides reference/value text for ADCs and their caps.

import pcbnew
import re
import wx
from typing import List, Dict, Optional, Set


# -----------------------------------------------------------------------------
# Dialog: ask only for pitch
# -----------------------------------------------------------------------------

class SensorPitchDialog(wx.Dialog):
    """
    Simple dialog to get sensor pitch in mm.
    """

    def __init__(self, parent=None):
        super().__init__(parent, title="PHOTON Layout Assistant Pitch")

        vbox = wx.BoxSizer(wx.VERTICAL)

        self.pitch_label = wx.StaticText(self, label="Sensor pitch (mm):")
        self.pitch_text = wx.TextCtrl(self, value="13.3333")

        grid = wx.FlexGridSizer(1, 2, 5, 5)
        grid.Add(self.pitch_label, 0, wx.ALIGN_RIGHT | wx.ALIGN_CENTER_VERTICAL)
        grid.Add(self.pitch_text, 1, wx.EXPAND)
        grid.AddGrowableCol(1, 1)

        vbox.Add(grid, 1, wx.ALL | wx.EXPAND, 10)

        btn_sizer = self.CreateSeparatedButtonSizer(wx.OK | wx.CANCEL)
        vbox.Add(btn_sizer, 0, wx.ALL | wx.ALIGN_CENTER_HORIZONTAL, 10)

        self.SetSizerAndFit(vbox)

    def get_pitch(self) -> float:
        """
        Return float(pitch_mm), or raise ValueError.
        """
        pitch = float(self.pitch_text.GetValue())
        if pitch <= 0:
            raise ValueError("Pitch must be > 0")
        return pitch


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def mm_to_pos(x_mm: float, y_mm: float) -> pcbnew.VECTOR2I:
    """Helper: create a KiCad position from mm coordinates."""
    return pcbnew.VECTOR2I_MM(x_mm, y_mm)


def mm_to_width(width_mm: float) -> int:
    """Helper: convert width in mm to internal units."""
    return pcbnew.FromMM(width_mm)


def pos_to_mm(pos: pcbnew.VECTOR2I) -> (float, float):
    """Convert a KiCad position to (x_mm, y_mm)."""
    return pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)


# -----------------------------------------------------------------------------
# Template coordinates for cluster 0 (bank 1) — ORIGINAL values
#
# For cluster n we add dx = n * pitch to all X coordinates.
# -----------------------------------------------------------------------------

# VCNT itself:
VCNT_BASE_X = 3.0
VCNT_BASE_Y = 2.4

# --- Pin 1 trace + via (to GND) ---
PIN1_TRACE_START = (2.4, 1.537)
PIN1_TRACE_END   = (0.9, 1.55)
PIN1_VIA_POS     = (0.9, 1.55)

# --- Pin 2 trace + resistor (R_pin2, connected to VCNT pin 2) ---
# From pin 2 of the VCNT:
#   0.15mm trace start: (2.399, 2.894) -> (0.81, 2.9)
#   end point is pad 2 of the resistor.
# Bank 1 resistor center: (0.3, 2.9), orientation 0°.
PIN2_TRACE_START = (2.399, 2.894)
PIN2_TRACE_END   = (0.81, 2.9)        # pad 2 of R_pin2 (bank 1)
R_PIN2_CENTER    = (0.8, 3.4)         # R_pin2 center (bank 1), 90°

# --- Pin 3 trace + resistor (R_pin3) ---
PIN3_TRACE_START = (3.685175, 1.63927)
PIN3_TRACE_END   = (5.18, 1.64)        # pad 2 of R_pin3
R_PIN3_CENTER    = (5.7, 1.65)         # R_pin3 center (bank 1), 180°

# --- Pin 4 trace + Q ---
PIN4_TRACE_START = (3.685175, 3.05913)
PIN4_TRACE_END   = (6.75, 3.06)        # pad 3 of Q
Q_CENTER         = (7.7, 3.05)         # Q center (bank 1), 180°

# --- Q pin1 to R_q1 ---
Q_PIN1_TRACE_START = (8.6375, 4.0)
Q_PIN1_TRACE_END   = (10.37, 4.0)      # pad 1 of R_q1
R_Q1_CENTER        = (10.38, 3.5)      # R_q1 center (bank 1), 90°

# --- Q pin2 to GND via ---
Q_PIN2_TRACE_START = (8.6375, 2.1)
Q_PIN2_TRACE_END   = (10.38, 2.1)      # near GND via

# --- GND via + traces using VCNT pin1's net (assumed GND) ---
GND_VIA_POS        = (10.41, 2.13)

GND_TRACE_1_START  = (10.38, 2.96)
GND_TRACE_1_END    = (10.38, 2.16)     # R_q1 pin down to via


# -----------------------------------------------------------------------------
# Plugin 1: sensor array placer & router
# -----------------------------------------------------------------------------

class VcntSensorArrayPlugin(pcbnew.ActionPlugin):
    """
    VCNT Sensor array placer/router using KiCad 9 API
    """

    def defaults(self):
        self.name = "PHOTON Layout Assistant"
        self.category = "Automation"
        self.description = "Place & route VCNT2025X01 sensor clusters along X with given pitch"
        self.show_toolbar_button = True
        self.icon_file_name = ""

    # -------------------------------------------------------------------------
    # Footprint / pad helpers
    # -------------------------------------------------------------------------

    def _is_vcnt_sensor(self, fp: pcbnew.FOOTPRINT) -> bool:
        """
        Detect VCNT sensors. Match on VALUE containing 'VCNT2025'.
        Adjust if you use a different value.
        """
        value = fp.GetValue().upper()
        return "VCNT2025" in value

    def _get_sorted_vcnt_sensors(self, board: pcbnew.BOARD) -> List[pcbnew.FOOTPRINT]:
        sensors = [fp for fp in board.GetFootprints() if self._is_vcnt_sensor(fp)]
        sensors.sort(
            key=lambda f: (
                self._sheet_id(f.GetReference()),
                self._ref_num(f.GetReference()),
                f.GetReference(),
            )
        )
        return sensors

    def _get_pad_by_number(self, fp: pcbnew.FOOTPRINT, pad_number: str) -> Optional[pcbnew.PAD]:
        for pad in fp.Pads():
            if pad.GetNumber() == pad_number:
                return pad
        return None

    def _ref_num(self, ref: str) -> int:
        match = re.search(r"\d+", ref)
        return int(match.group()) if match else 0

    def _sheet_id(self, ref: str) -> int:
        return self._ref_num(ref) // 100

    def _find_fp_by_ref(self, board: pcbnew.BOARD, ref: str) -> Optional[pcbnew.FOOTPRINT]:
        for fp in board.GetFootprints():
            if fp.GetReference() == ref:
                return fp
        return None

    def _is_adc(self, fp: pcbnew.FOOTPRINT) -> bool:
        value = fp.GetValue().upper()
        return "TLA2518" in value

    def _get_sorted_adcs(self, board: pcbnew.BOARD) -> List[pcbnew.FOOTPRINT]:
        adcs = [fp for fp in board.GetFootprints() if self._is_adc(fp)]
        adcs.sort(
            key=lambda f: (
                self._sheet_id(f.GetReference()),
                self._ref_num(f.GetReference()),
                f.GetReference(),
            )
        )
        return adcs

    def _hide_fp_silkscreen_text(self, fp: pcbnew.FOOTPRINT) -> None:
        """Hide reference/value text and move it off F.Silk."""
        if hasattr(fp, "SetReferenceVisible"):
            fp.SetReferenceVisible(False)
        elif hasattr(fp, "SetReferenceVisibility"):
            fp.SetReferenceVisibility(False)

        if hasattr(fp, "SetValueVisible"):
            fp.SetValueVisible(False)
        elif hasattr(fp, "SetValueVisibility"):
            fp.SetValueVisibility(False)

        for text in (fp.Reference(), fp.Value()):
            if text is None:
                continue
            if hasattr(text, "SetLayer"):
                text.SetLayer(pcbnew.F_Fab)
            if hasattr(text, "SetVisible"):
                text.SetVisible(False)
            elif hasattr(text, "SetVisibility"):
                text.SetVisibility(False)

    # -------------------------------------------------------------------------
    # Routing primitives
    # -------------------------------------------------------------------------

    def _create_track_mm(
        self,
        board: pcbnew.BOARD,
        net_code: int,
        x1_mm: float,
        y1_mm: float,
        x2_mm: float,
        y2_mm: float,
        width_mm: float,
        layer: int = pcbnew.F_Cu,
    ) -> pcbnew.PCB_TRACK:
        """
        Create a straight track segment from (x1, y1) to (x2, y2) in mm,
        on the given layer, assigned to net_code.
        """
        track = pcbnew.PCB_TRACK(board)
        track.SetStart(mm_to_pos(x1_mm, y1_mm))
        track.SetEnd(mm_to_pos(x2_mm, y2_mm))
        track.SetWidth(mm_to_width(width_mm))
        track.SetLayer(layer)
        track.SetNetCode(net_code)
        board.Add(track)
        return track

    def _create_via_mm(
        self,
        board: pcbnew.BOARD,
        net_code: int,
        x_mm: float,
        y_mm: float,
        drill_mm: float = 0.3,
        diameter_mm: float = 0.6,
    ) -> pcbnew.PCB_VIA:
        """
        Create a via at (x, y) in mm with given drill and diameter, assigned to net_code.
        """
        via = pcbnew.PCB_VIA(board)
        via.SetPosition(mm_to_pos(x_mm, y_mm))
        via.SetDrill(mm_to_width(drill_mm))
        via.SetWidth(mm_to_width(diameter_mm))
        via.SetNetCode(net_code)
        board.Add(via)
        return via

    # -------------------------------------------------------------------------
    # Resolve cluster components by nets
    # -------------------------------------------------------------------------

    def _resolve_cluster_components(
        self,
        board: pcbnew.BOARD,
        sensor_fp: pcbnew.FOOTPRINT,
        used_r_refs: Set[str],
        used_q_refs: Set[str],
    ) -> Dict[str, Optional[pcbnew.FOOTPRINT]]:
        """
        For a given VCNT footprint, find its associated R/Q parts based on nets.

        Returns a dict with keys:
            - "R_pin3" : resistor connected to VCNT pin3 net
            - "R_pin2" : resistor connected to VCNT pin2 net
            - "Q"      : transistor whose pad3 == VCNT pin4 net
            - "R_q1"   : resistor sharing net with Q pad1
        """
        result: Dict[str, Optional[pcbnew.FOOTPRINT]] = {
            "R_pin3": None,
            "R_pin2": None,
            "Q": None,
            "R_q1": None,
        }

        pad1 = self._get_pad_by_number(sensor_fp, "1")
        pad2 = self._get_pad_by_number(sensor_fp, "2")
        pad3 = self._get_pad_by_number(sensor_fp, "3")
        pad4 = self._get_pad_by_number(sensor_fp, "4")

        if not (pad1 and pad2 and pad3 and pad4):
            return result

        net1 = pad1.GetNetCode()
        net2 = pad2.GetNetCode()
        net3 = pad3.GetNetCode()
        net4 = pad4.GetNetCode()

        # --- First: find R_pin3 (resistor on net3) ---
        for fp in board.GetFootprints():
            ref = fp.GetReference()
            if ref in used_r_refs:
                continue
            if not ref.startswith("R"):
                continue

            nets = {p.GetNetCode() for p in fp.Pads()}
            if net3 in nets and result["R_pin3"] is None:
                result["R_pin3"] = fp
                used_r_refs.add(ref)

        # --- Second: find Q (transistor with pad3 == net4) ---
        for fp in board.GetFootprints():
            ref = fp.GetReference()
            if ref in used_q_refs:
                continue
            if not ref.startswith("Q"):
                continue

            q_pad3 = self._get_pad_by_number(fp, "3")
            if q_pad3 and q_pad3.GetNetCode() == net4:
                result["Q"] = fp
                used_q_refs.add(ref)
                break

        # --- Third: find R_q1 (resistor sharing net with Q pad1) ---
        if result["Q"] is not None:
            q_pad1 = self._get_pad_by_number(result["Q"], "1")
            if q_pad1:
                net_q1 = q_pad1.GetNetCode()
                for fp in board.GetFootprints():
                    ref = fp.GetReference()
                    if ref in used_r_refs:
                        continue
                    if not ref.startswith("R"):
                        continue
                    nets = {p.GetNetCode() for p in fp.Pads()}
                    if net_q1 in nets:
                        result["R_q1"] = fp
                        used_r_refs.add(ref)
                        break

        # --- Finally: find R_pin2 (resistor on VCNT pin2 net) ---
        for fp in board.GetFootprints():
            ref = fp.GetReference()
            if ref in used_r_refs:
                continue
            if not ref.startswith("R"):
                continue

            nets = {p.GetNetCode() for p in fp.Pads()}
            if net2 in nets:
                result["R_pin2"] = fp
                used_r_refs.add(ref)
                break

        return result

    # -------------------------------------------------------------------------
    # Place components for one cluster (VCNT + associated R/Q)
    # -------------------------------------------------------------------------

    def _place_cluster_components(
        self,
        sensor_fp: pcbnew.FOOTPRINT,
        cluster: Dict[str, Optional[pcbnew.FOOTPRINT]],
        sensor_index: int,
        pitch_mm: float,
    ):
        """
        Place VCNT + its Rs & Q using template coordinates shifted by index * pitch.
        """
        dx = sensor_index * pitch_mm

        # Place the VCNT itself
        sensor_fp.SetPosition(mm_to_pos(VCNT_BASE_X + dx, VCNT_BASE_Y))
        sensor_fp.SetOrientationDegrees(0.0)
        self._hide_fp_silkscreen_text(sensor_fp)

        # R connected to VCNT pin3 (R_pin3), 180°
        r_pin3 = cluster["R_pin3"]
        if r_pin3 is not None:
            cx, cy = R_PIN3_CENTER
            r_pin3.SetPosition(mm_to_pos(cx + dx, cy))
            r_pin3.SetOrientationDegrees(180.0)
            self._hide_fp_silkscreen_text(r_pin3)

        # R connected to VCNT pin2 (R_pin2), 90°
        r_pin2 = cluster["R_pin2"]
        if r_pin2 is not None:
            cx, cy = R_PIN2_CENTER
            r_pin2.SetPosition(mm_to_pos(cx + dx, cy))
            r_pin2.SetOrientationDegrees(90.0)
            self._hide_fp_silkscreen_text(r_pin2)

        # Q, 180°
        q_fp = cluster["Q"]
        if q_fp is not None:
            cx, cy = Q_CENTER
            q_fp.SetPosition(mm_to_pos(cx + dx, cy))
            q_fp.SetOrientationDegrees(180.0)
            self._hide_fp_silkscreen_text(q_fp)

        # R connected to Q pin1 (R_q1), 90°
        r_q1 = cluster["R_q1"]
        if r_q1 is not None:
            cx, cy = R_Q1_CENTER
            r_q1.SetPosition(mm_to_pos(cx + dx, cy))
            r_q1.SetOrientationDegrees(90.0)
            self._hide_fp_silkscreen_text(r_q1)

    # -------------------------------------------------------------------------
    # Routing pattern for one cluster
    # -------------------------------------------------------------------------

    def _route_cluster_pattern(
        self,
        board: pcbnew.BOARD,
        sensor_fp: pcbnew.FOOTPRINT,
        cluster: Dict[str, Optional[pcbnew.FOOTPRINT]],
        sensor_index: int,
        pitch_mm: float,
    ):
        """
        Route all the tracks/vias for this VCNT cluster using the template,
        shifted by index * pitch.
        """
        dx = sensor_index * pitch_mm
        width_mm = 0.15

        pad1 = self._get_pad_by_number(sensor_fp, "1")
        pad2 = self._get_pad_by_number(sensor_fp, "2")
        pad3 = self._get_pad_by_number(sensor_fp, "3")
        pad4 = self._get_pad_by_number(sensor_fp, "4")

        if not (pad1 and pad2 and pad3 and pad4):
            return

        net1 = pad1.GetNetCode()  # assumed GND
        net2 = pad2.GetNetCode()
        net3 = pad3.GetNetCode()
        net4 = pad4.GetNetCode()

        # --- Pin 1 trace + via (to GND) ---
        self._create_track_mm(
            board,
            net1,
            PIN1_TRACE_START[0] + dx,
            PIN1_TRACE_START[1],
            PIN1_TRACE_END[0] + dx,
            PIN1_TRACE_END[1],
            width_mm,
        )
        self._create_via_mm(
            board,
            net1,
            PIN1_VIA_POS[0] + dx,
            PIN1_VIA_POS[1],
            drill_mm=0.3,
            diameter_mm=0.6,
        )

        # --- Pin 2 trace to its resistor (R_pin2) ---
        self._create_track_mm(
            board,
            net2,
            PIN2_TRACE_START[0] + dx,
            PIN2_TRACE_START[1],
            PIN2_TRACE_END[0] + dx,
            PIN2_TRACE_END[1],
            width_mm,
        )

        # --- Pin 3 trace to its resistor (R_pin3) ---
        self._create_track_mm(
            board,
            net3,
            PIN3_TRACE_START[0] + dx,
            PIN3_TRACE_START[1],
            PIN3_TRACE_END[0] + dx,
            PIN3_TRACE_END[1],
            width_mm,
        )

        # --- Pin 4 trace to Q pin3 ---
        self._create_track_mm(
            board,
            net4,
            PIN4_TRACE_START[0] + dx,
            PIN4_TRACE_START[1],
            PIN4_TRACE_END[0] + dx,
            PIN4_TRACE_END[1],
            width_mm,
        )

        # --- Q pin1 to R_q1 ---
        q_fp = cluster["Q"]
        r_q1 = cluster["R_q1"]
        if q_fp is not None and r_q1 is not None:
            q_pad1 = self._get_pad_by_number(q_fp, "1")
            if q_pad1:
                net_q1 = q_pad1.GetNetCode()
                self._create_track_mm(
                    board,
                    net_q1,
                    Q_PIN1_TRACE_START[0] + dx,
                    Q_PIN1_TRACE_START[1],
                    Q_PIN1_TRACE_END[0] + dx,
                    Q_PIN1_TRACE_END[1],
                    width_mm,
                )

        # --- Q pin2 to GND via ---
        if q_fp is not None:
            q_pad2 = self._get_pad_by_number(q_fp, "2")
            if q_pad2:
                net_q2 = q_pad2.GetNetCode()
                self._create_track_mm(
                    board,
                    net_q2,
                    Q_PIN2_TRACE_START[0] + dx,
                    Q_PIN2_TRACE_START[1],
                    Q_PIN2_TRACE_END[0] + dx,
                    Q_PIN2_TRACE_END[1],
                    width_mm,
                )

        # --- GND via + traces (using net1 as GND) ---
        self._create_via_mm(
            board,
            net1,
            GND_VIA_POS[0] + dx,
            GND_VIA_POS[1],
            drill_mm=0.3,
            diameter_mm=0.6,
        )

        # via -> R_q1 vertical
        self._create_track_mm(
            board,
            net1,
            GND_TRACE_1_START[0] + dx,
            GND_TRACE_1_START[1],
            GND_TRACE_1_END[0] + dx,
            GND_TRACE_1_END[1],
            width_mm,
        )

    # -------------------------------------------------------------------------
    # ADC cluster placement
    # -------------------------------------------------------------------------

    def _place_adc_clusters(self, board: pcbnew.BOARD, pitch_mm: float) -> int:
        adcs = self._get_sorted_adcs(board)
        count = 0
        for idx, adc in enumerate(adcs):
            adc_ref = adc.GetReference()
            adc_num = self._ref_num(adc_ref)

            dx = idx * pitch_mm * 4.0
            adc_x = ADC_BASE_X + dx
            adc_y = ADC_BASE_Y

            adc.SetPosition(mm_to_pos(adc_x, adc_y))
            adc.SetOrientationDegrees(ADC_ORIENTATION_DEG)
            self._hide_fp_silkscreen_text(adc)

            for offset_idx, cap_dx, cap_dy, cap_rot in ADC_CAP_OFFSETS:
                cap_ref = f"C{adc_num + offset_idx}"
                cap = self._find_fp_by_ref(board, cap_ref)
                if cap is None:
                    continue
                cap.SetPosition(mm_to_pos(adc_x + cap_dx, adc_y + cap_dy))
                cap.SetOrientationDegrees(cap_rot)
                self._hide_fp_silkscreen_text(cap)

            count += 1

        return count

    def _add_vcnt_labels(self, board: pcbnew.BOARD, sensors: List[pcbnew.FOOTPRINT]) -> int:
        text_height_mm = 1.0
        count = 0

        for idx, sensor in enumerate(sensors):
            pos = sensor.GetPosition()
            x_mm, y_mm = pos_to_mm(pos)
            y_text = y_mm + 2.5

            text = pcbnew.PCB_TEXT(board)
            text.SetText(str(idx))
            text.SetPosition(mm_to_pos(x_mm, y_text))
            text.SetLayer(pcbnew.F_SilkS)

            size_internal = mm_to_width(text_height_mm)
            if hasattr(text, "SetTextHeight"):
                text.SetTextHeight(size_internal)
            if hasattr(text, "SetTextWidth"):
                text.SetTextWidth(size_internal)

            try:
                text.SetHorizJustify(pcbnew.GR_TEXT_HJUSTIFY_CENTER)
                text.SetVertJustify(pcbnew.GR_TEXT_VJUSTIFY_CENTER)
            except AttributeError:
                pass

            board.Add(text)
            count += 1

        return count

    # -------------------------------------------------------------------------
    # Main entry point
    # -------------------------------------------------------------------------

    def Run(self):
        board = pcbnew.GetBoard()
        if board is None:
            wx.MessageBox("No board is currently open.", "Error", wx.OK | wx.ICON_ERROR)
            return

        # Ask for pitch only
        dlg = SensorPitchDialog()
        if dlg.ShowModal() != wx.ID_OK:
            dlg.Destroy()
            return

        try:
            pitch_mm = dlg.get_pitch()
        except ValueError as e:
            wx.MessageBox(str(e), "Invalid input", wx.OK | wx.ICON_ERROR)
            dlg.Destroy()
            return

        dlg.Destroy()

        sensors = self._get_sorted_vcnt_sensors(board)
        if not sensors:
            wx.MessageBox(
                "No VCNT2025X01 sensors (value containing 'VCNT2025') found on board.",
                "Nothing to do",
                wx.OK | wx.ICON_INFORMATION,
            )
            return

        used_r_refs: Set[str] = set()
        used_q_refs: Set[str] = set()

        for idx, sensor in enumerate(sensors):
            cluster = self._resolve_cluster_components(board, sensor, used_r_refs, used_q_refs)

            # Place all parts for this sensor
            self._place_cluster_components(sensor, cluster, idx, pitch_mm)

            # Route all tracks/vias for this sensor
            self._route_cluster_pattern(board, sensor, cluster, idx, pitch_mm)

        adc_count = self._place_adc_clusters(board, pitch_mm)
        self._add_vcnt_labels(board, sensors)

        pcbnew.Refresh()
        adc_msg = (
            f"\nPlaced {adc_count} ADC clusters."
            "\nFully route/layout bank 1, then use Replicate Layout to copy to the remaining banks."
            if adc_count else ""
        )
        wx.MessageBox(
            f"Processed {len(sensors)} VCNT sensors with pitch {pitch_mm:.4f} mm.\n"
            "VCNT + R/Q clusters placed and routed." + adc_msg,
            "PHOTON Layout Assistant",
            wx.OK | wx.ICON_INFORMATION,
        )


# --------------------------------------------------------------------
# ADC placement template (used by VCNT placer)
# --------------------------------------------------------------------

# Base positions (bank 0 / U401) in mm: absolute coords from your layout
ADC_BASE_X = 23.88
ADC_BASE_Y = 10.23
ADC_ORIENTATION_DEG = -90.0

# Offsets for Cx01..Cx07 relative to ADC center (from U401 layout)
ADC_CAP_OFFSETS = [
    (0, -3.289999, 0.25, 180.0),   # Cx01 (C401)
    (1, -3.279999, 1.30, 180.0),   # Cx02 (C402)
    (2, -0.249999, 3.59, -90.0),   # Cx03 (C403)
    (3, -4.349999, -2.61, 180.0),  # Cx04 (C404)
    (4, -0.939999, -3.47, 90.0),   # Cx05 (C405)
    (5, 0.460001, -3.47, 90.0),    # Cx06 (C406)
    (6, 3.670001, -0.73, 0.0),     # Cx07 (C407)
]



# Register plugin with KiCad
VcntSensorArrayPlugin().register()

#!/usr/bin/env python3
"""Measure a Cornell --software-gi-self-test capture with hardware tracing off."""

import argparse
import json
import math
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as error:
    DEPENDENCY_ERROR = str(error)
else:
    DEPENDENCY_ERROR = None
    LUMA = np.array((0.2126, 0.7152, 0.0722), dtype=np.float32)


FRAMES = (1, 4, 8, 32, 64)
GRAIN_WINDOWS = (5, 9, 17)
PLANES = {
    "ceiling": (128, 0, 128),
    "floor": (128, 255, 128),
    "backWall": (128, 128, 255),
}
COMPARABLE_SETTINGS = ("width", "height", "antiAliasing", "diffuseIntensity", "lightEdit")
BUDGET_SETTINGS = (
    "probeGridBudget", "probeRayBudget", "probeHysteresis", "ssgiRayBudget", "ssgiStepBudget", "screenSpaceRadius",
)
# The fixed Cornell fixture establishes these limits in both display codes and
# fractions of mean plane luma. The columns are frames 1, 4, 8, and 64.
CURVATURE_FRAMES = (1, 4, 8, 64)
CURVATURE_METRICS = (("8", "luma"), ("32", "luma"), ("32", "chroma"))
CURVATURE_LIMITS = {
    "ddgi": ((0.43, 0.26, 0.28, 0.19), (0.66, 0.66, 0.47, 0.46), (0.16, 0.14, 0.14, 0.14)),
    "ssgi": ((0.10, 0.08, 0.07, 0.06), (0.21, 0.18, 0.16, 0.16), (0.08, 0.08, 0.08, 0.08)),
}
CURVATURE_LIMITS_CODES = ((11.0, 6.0, 7.0, 4.5), (18.0, 18.0, 14.0, 14.0), (5.0, 4.5, 4.5, 4.5))
DRIFT_LIMITS = {1: (0.13, 0.42), 4: (0.08, 0.23), 8: (0.065, 0.20), 32: (0.035, 0.09)}
DRIFT_LIMITS_CODES = {1: (5.8, 11.5), 4: (4.5, 8.0), 8: (3.4, 6.5), 32: (1.4, 2.7)}
EDIT_COMPLETION_MINIMUM = {1: 0.50, 4: 0.65, 8: 0.70}
MIN_EDIT_SIGNAL_CODES = 4.0
MIN_PLANE_LUMA = {
    "ddgi": {"ceiling": 15.0, "floor": 26.0, "backWall": 28.0},
    "ssgi": {"ceiling": 38.0, "floor": 20.0, "backWall": 36.0},
}
MIN_BASELINE_LUMA_RATIO = 0.90
# Cornell ceiling at scene exposure. Values are local luma residual P95 in
# display codes for 9 and 17 px windows, with margin across raster backends.
CEILING_GRAIN_LIMITS = {
    "ddgi": {"diffuse": (0.59, 1.20), "light-edit": (0.68, 1.35),
             "lit": (0.80, 1.25), "specular": (1.20, 1.75)},
    "ssgi": {"diffuse": (0.88, 1.62), "light-edit": (0.94, 1.75),
             "lit": (0.90, 1.70), "specular": (0.95, 1.25)},
}
ORBIT_CEILING_GRAIN_LIMIT = {"ddgi": 1.02, "ssgi": 1.06}
ORBIT_CEILING_DRIFT_LIMIT = {"ddgi": 1.0, "ssgi": 1.75}


class Captures:
    def __init__(self, directory):
        self.directory = directory
        self.metadata = json.loads((directory / "software-gi.json").read_text(encoding="utf-8"))
        if (self.metadata["passed"] is not True or self.metadata["validationErrors"] != 0
                or self.metadata["dxrDispatches"] != 0):
            raise ValueError(f"{directory} needs a passing capture with zero validation errors and zero DXR dispatches")
        self.size = self.metadata["width"], self.metadata["height"]

    def has(self, name):
        return name in self.metadata["captures"]

    def image(self, name, frame, view):
        capture = self.metadata["captures"][name]
        if capture["frame"] != frame or capture["view"] != view:
            raise ValueError(f"{self.directory}/{name} has unexpected frame or debug view")
        with Image.open(self.directory / capture["image"]) as image:
            if image.size != self.size:
                raise ValueError(f"{self.directory}/{name} is {image.size}, expected {self.size}")
            return np.asarray(image.convert("RGB"))


def plane_masks(normals, min_pixels=10_000):
    normals = normals.astype(np.int16)
    masks = {}
    border = 12
    for name, code in PLANES.items():
        mask = (np.abs(normals - code) <= 1).all(axis=-1)
        # Keep samples at least 12 px inside the same axis-aligned plane.
        sums = np.pad(mask, ((1, 0), (1, 0))).cumsum(0, dtype=np.int32).cumsum(1, dtype=np.int32)
        width = 2 * border + 1
        interior = sums[width:, width:] - sums[:-width, width:] - sums[width:, :-width] + sums[:-width, :-width]
        safe = np.pad(interior == width * width, border)
        if safe.sum() < min_pixels:
            raise ValueError(f"Normals capture has too few interior {name} pixels for the Cornell fixture")
        masks[name] = safe
    return masks


def tiles(image, size):
    height, width = image.shape[:2]
    rows, columns = height // size, width // size
    return image[:rows * size, :columns * size].reshape((rows, size, columns, size) + image.shape[2:])


def curvature_p95(values, mask):
    horizontal = mask[:, 1:-1] & mask[:, :-2] & mask[:, 2:]
    vertical = mask[1:-1] & mask[:-2] & mask[2:]
    dx = values[:, :-2] - 2 * values[:, 1:-1] + values[:, 2:]
    dy = values[:-2] - 2 * values[1:-1] + values[2:]
    differences = np.concatenate((dx[horizontal], dy[vertical]))
    if differences.size < 10:
        raise ValueError("Cornell plane has too few fully covered tile triplets for curvature")
    return float(np.percentile(np.abs(differences), 95))


def absolute_difference(values):
    values = np.abs(values)
    return {"mae": float(values.mean()), "p95": float(np.percentile(values, 95))}


def measure_grain(image, masks, reference=None):
    luma = image @ LUMA
    integral = np.pad(luma, ((1, 0), (1, 0))).cumsum(0, dtype=np.float64).cumsum(1)
    result = {}
    for plane, mask in masks.items():
        mean_luma = float(luma[mask].mean())
        row = {"meanLuma": mean_luma, "pixelCount": int(mask.sum()), "windows": {}}
        if reference is not None:
            delta = image[mask].astype(np.float32) - reference[mask]
            row["lumaDifferenceFromRest"] = absolute_difference(delta @ LUMA)
            row["rgbDifferenceFromRest"] = absolute_difference(delta)
        result[plane] = row

    # The masks already exclude a 12 px border within each plane. All taps in
    # these smaller windows belong to the same plane as the tested pixel.
    for size in GRAIN_WINDOWS:
        radius = size // 2
        window_mean = (integral[size:, size:] - integral[:-size, size:]
                       - integral[size:, :-size] + integral[:-size, :-size]) / (size * size)
        high_pass = np.abs(luma[radius:-radius, radius:-radius] - window_mean)
        for plane, mask in masks.items():
            values = high_pass[mask[radius:-radius, radius:-radius]]
            p95, p99 = np.percentile(values, (95, 99))
            row = result[plane]
            row["windows"][str(size)] = {"lumaResidualP95": float(p95), "lumaResidualP99": float(p99),
                                         "lumaResidualP95OverMeanLuma": float(p95) / max(row["meanLuma"], 1.0)}
    return result


def orbit_key(method):
    return f"{method}-orbit-normals"


def measure_orbit(captures, method):
    prefix = f"{method}-orbit"
    normals_key = orbit_key(method)
    moving_key = prefix + "-16"
    settled_key = prefix + "-rest"
    if not any(captures.has(key) for key in (normals_key, moving_key, settled_key)):
        return None
    masks = plane_masks(captures.image(normals_key, 1, 3), min_pixels=2000)
    moving = captures.image(moving_key, 16, 0)
    settled = captures.image(settled_key, 64, 0)
    return {"moving": measure_grain(moving, masks, reference=settled),
            "settled": measure_grain(settled, masks)}


def measure_disocclusion(captures, method):
    if not captures.has(f"{method}-revealed-normals"):
        return None
    old_normals = captures.image(f"{method}-occluded-normals", 1, 3).astype(np.int16)
    normals = captures.image(f"{method}-revealed-normals", 1, 3)
    # A previously curved receiver has moved away and exposed a planar wall.
    mask = (np.abs(normals.astype(np.int16) - old_normals).max(axis=2) > 16)
    mask &= plane_masks(normals)["backWall"]
    if mask.sum() < 400:
        raise ValueError("The moving sphere did not expose a measurable back-wall region")
    reference = captures.image(f"{method}-revealed-64", 64, 0)[mask].astype(np.float32)
    before = captures.image(f"{method}-occluded", 32, 0)[mask].astype(np.float32)
    result = {"pixels": int(mask.sum()), "poseDifference": absolute_difference(before - reference)}
    for frame in (1, 4):
        current = captures.image(f"{method}-revealed-{frame}", frame, 0)[mask].astype(np.float32)
        result[str(frame)] = absolute_difference(current - reference)
    return result


def back_wall_corner(captures):
    normals = captures.image("normals", 1, 3).astype(np.int16)
    back = (np.abs(normals - PLANES["backWall"]) <= 1).all(axis=-1)
    side = (np.abs(normals - (255, 128, 128)) <= 1).all(axis=-1)
    # Follow each visible shared edge onto the back wall. Both samples must
    # lie on the same uninterrupted receiver, including at higher resolutions.
    y, x = np.nonzero(side[:, :-1] & back[:, 1:])
    width = normals.shape[1]
    near, far = round(width * 0.005), round(width * 0.025)
    inside = x + far < width
    y, x = y[inside], x[inside]
    continuous = np.ones(x.size, dtype=bool)
    for offset in range(1, far + 1):
        continuous &= back[y, x + offset]
    y, x = y[continuous], x[continuous]
    if x.size < 100:
        raise ValueError("Cornell corner has too few uninterrupted back-wall rows")
    return y, x + near, x + far


def measure_plane(image, settled, before_edit, mask):
    mean_rgb = image[mask].mean(axis=0)
    mean_luma = float(mean_rgb @ LUMA)
    result = {"meanRgb": mean_rgb.tolist(), "meanLuma": mean_luma, "pixelCount": int(mask.sum()), "tiles": {}}
    for size in (8, 32):
        usable = tiles(mask, size).all(axis=(1, 3))
        rgb = tiles(image, size).mean(axis=(1, 3), dtype=np.float32)
        reference = tiles(settled, size).mean(axis=(1, 3), dtype=np.float32)
        luma = rgb @ LUMA
        chroma = rgb[..., (0, 2)] - rgb[..., 1, None]
        luma_curvature = curvature_p95(luma, usable)
        chroma_curvature = curvature_p95(chroma, usable)
        row = {
            "tileCount": int(usable.sum()),
            "lumaCurvatureP95": luma_curvature,
            "chromaCurvatureP95": chroma_curvature,
            "lumaCurvatureP95OverMeanLuma": luma_curvature / max(mean_luma, 1.0),
            "chromaCurvatureP95OverMeanLuma": chroma_curvature / max(mean_luma, 1.0),
            "lumaDifferenceFromSettled": absolute_difference(((rgb - reference) @ LUMA)[usable]),
            "rgbDifferenceFromSettled": absolute_difference((rgb - reference)[usable]),
        }
        if before_edit is not None:
            before = tiles(before_edit, size).mean(axis=(1, 3), dtype=np.float32)
            full_edit = absolute_difference((reference - before)[usable])
            row["settledRgbDifferenceFromBeforeEdit"] = full_edit
            row["editCompletion"] = (1.0 - row["rgbDifferenceFromSettled"]["mae"] / full_edit["mae"]
                                     if full_edit["mae"] > 0.0 else None)
        result["tiles"][str(size)] = row
    return result


def measure(captures, masks):
    result = {"directory": str(captures.directory), "adapter": captures.metadata.get("adapter"),
              "settings": {name: captures.metadata.get(name) for name in COMPARABLE_SETTINGS + BUDGET_SETTINGS},
              "methods": {}}
    rows, near, far = back_wall_corner(captures)
    for method in ("ddgi", "ssgi"):
        mode = result["methods"][method] = {"gpuTiming": captures.metadata["timing"][method], "grain": {}}
        before = captures.image(f"{method}-diffuse-f064", 64, 11)
        gradient = before[rows, near].astype(np.float32) - before[rows, far]
        mode["corner"] = {"rows": int(rows.size), "nearMinusFarLuma": float((gradient @ LUMA).mean())}
        for group in ("diffuse", "light-edit"):
            settled = before if group == "diffuse" else captures.image(f"{method}-{group}-f064", 64, 11)
            mode[group] = {}
            mode["grain"][group] = {}
            for frame in FRAMES:
                image = settled if frame == 64 else captures.image(f"{method}-{group}-f{frame:03}", frame, 11)
                mode[group][str(frame)] = {
                    name: measure_plane(image, settled, before if group == "light-edit" else None, mask)
                    for name, mask in masks.items()
                }
                mode["grain"][group][str(frame)] = measure_grain(image, masks)
        for frame in (1, 64):
            key = f"{method}-lit-f{frame:03}"
            if captures.has(key):
                mode["grain"].setdefault("lit", {})[str(frame)] = measure_grain(
                    captures.image(key, frame, 0), masks)
        key = f"{method}-specular"
        if captures.has(key):
            mode["grain"]["specular"] = {"1": measure_grain(captures.image(key, 1, 11), masks)}
        orbit = measure_orbit(captures, method)
        if orbit is not None:
            mode["orbit"] = orbit
        reveal = measure_disocclusion(captures, method)
        if reveal is not None:
            mode["disocclusion"] = reveal
    return result


def grain_errors(method, mode, baseline):
    errors = []
    if baseline:
        reference = baseline["corner"]["nearMinusFarLuma"]
        if abs(mode["corner"]["nearMinusFarLuma"] - reference) > max(0.5, abs(reference) * 0.15):
            errors.append(f"{method} indirect lighting gradient changed along the back-wall corner")
    reveal = mode.get("disocclusion")
    if reveal:
        if reveal["poseDifference"]["mae"] < 5.0:
            errors.append(f"{method} moving sphere produced insufficient contrast")
        for frame in ("1", "4"):
            for metric in ("mae", "p95"):
                if reveal[frame][metric] > 0.3 * reveal["poseDifference"][metric]:
                    errors.append(f"{method} newly revealed wall f{frame} {metric} retained too much of the previous receiver")

    def check_grain(label, row, window, limit):
        metric = row["windows"][window]["lumaResidualP95"]
        if metric > limit:
            errors.append(f"{label} {window} px luma residual P95 {metric:.3f} exceeds {limit:.3f}")

    def check_luma(label, measured, reference):
        minimum = MIN_BASELINE_LUMA_RATIO * reference["meanLuma"]
        if measured["meanLuma"] < minimum:
            errors.append(f"{label} mean luma {measured['meanLuma']:.2f} is below {minimum:.2f}")

    for group, limits in CEILING_GRAIN_LIMITS[method].items():
        series = mode["grain"].get(group, {})
        if "1" in series:
            for window, limit in zip(("9", "17"), limits):
                check_grain(f"{method} {group} f1 ceiling", series["1"]["ceiling"], window, limit)
    if baseline:
        before_lit = baseline["grain"].get("lit", {})
        for frame, planes in mode["grain"].get("lit", {}).items():
            if frame in before_lit:
                for plane, row in planes.items():
                    check_luma(f"{method} lit f{frame} {plane}", row, before_lit[frame][plane])

    if "orbit" in mode:
        orbit = mode["orbit"]
        moving = orbit["moving"]["ceiling"]
        check_grain(f"{method} orbit ceiling", moving, "9", ORBIT_CEILING_GRAIN_LIMIT[method])
        drift = moving["lumaDifferenceFromRest"]["mae"]
        drift_limit = ORBIT_CEILING_DRIFT_LIMIT[method]
        if drift > drift_limit:
            errors.append(f"{method} orbit ceiling moving-to-rest luma MAE {drift:.3f} exceeds {drift_limit:.3f}")
        if baseline and "orbit" in baseline:
            for phase, planes in orbit.items():
                for plane, row in planes.items():
                    check_luma(f"{method} orbit {phase} {plane}", row, baseline["orbit"][phase][plane])
    return errors


def quality_errors(candidate, baseline):
    settings = candidate["settings"]
    resolution = (settings["width"], settings["height"])
    if resolution not in ((1281, 721), (1920, 1080), (2560, 1440)) or settings["antiAliasing"] != "FXAA":
        return ["Quality limits require the Cornell FXAA fixture at its default size, 1080p or 1440p"]
    if resolution != (1281, 721) and baseline is None:
        return ["1080p and 1440p checks need --baseline at the same resolution"]
    fixture = {"probeGridBudget": [30, 30, 30], "probeRayBudget": 32, "probeHysteresis": 0.89,
               "ssgiRayBudget": 4, "ssgiStepBudget": 8, "screenSpaceRadius": 2.0, "diffuseIntensity": 3.61,
               "lightEdit": {"horizontalOffsetMetres": 0.5, "radianceMultiplier": 1.65}}
    for capture in (candidate, baseline):
        if capture is None:
            continue
        for name, expected in fixture.items():
            actual = capture["settings"][name]
            if isinstance(expected, float) and isinstance(actual, (int, float)):
                if math.isclose(actual, expected, abs_tol=1e-4):
                    continue
            if actual != expected:
                return [f"Quality limits require {name} = {expected}, found {actual} in {capture['directory']}"]
    errors = []

    def check_upper(label, value, limit):
        if value > limit:
            errors.append(f"{label} {value:.3f} exceeds {limit:.3f}")

    for method, mode in candidate["methods"].items():
        limits = CURVATURE_LIMITS[method]
        errors.extend(grain_errors(method, mode, baseline["methods"][method] if baseline else None))
        if resolution != (1281, 721):
            # The same 32 px window subtends a different angle at these sizes.
            # Compare light response and retained energy at matched scale.
            reference = baseline["methods"][method]
            for plane in PLANES:
                for group in ("diffuse", "light-edit"):
                    for frame in ("1", "64"):
                        luma = mode[group][frame][plane]["meanLuma"]
                        previous = reference[group][frame][plane]["meanLuma"]
                        if luma < MIN_BASELINE_LUMA_RATIO * previous:
                            errors.append(f"{method} {group} f{frame} lost light on {plane}")
                for frame, minimum in EDIT_COMPLETION_MINIMUM.items():
                    response = mode["light-edit"][str(frame)][plane]["tiles"]["32"]["editCompletion"]
                    if response is None or response < minimum:
                        errors.append(f"{method} light edit f{frame} failed on {plane}")
            continue
        for group in ("diffuse", "light-edit"):
            sequence = mode[group]
            for frame in FRAMES:
                for plane, row in sequence[str(frame)].items():
                    label = f"{method} {group} f{frame} {plane}"
                    min_luma = MIN_PLANE_LUMA[method][plane]
                    if baseline:
                        original = baseline["methods"][method][group][str(frame)][plane]
                        min_luma = max(min_luma, MIN_BASELINE_LUMA_RATIO * original["meanLuma"])
                    if row["meanLuma"] < min_luma:
                        errors.append(f"{label} mean luma {row['meanLuma']:.2f} is below {min_luma:.2f}")

                    broad = row["tiles"]["32"]
                    if frame in CURVATURE_FRAMES:
                        index = CURVATURE_FRAMES.index(frame)
                        for (size, channel), relative, absolute in zip(CURVATURE_METRICS, limits, CURVATURE_LIMITS_CODES):
                            band = row["tiles"][size]
                            key = f"{channel}CurvatureP95"
                            metric = f"{label} {size} px {channel} curvature P95"
                            check_upper(f"{metric} in display codes", band[key], absolute[index])
                            check_upper(f"{metric} / mean luma", band[key + "OverMeanLuma"], relative[index])

                    if frame in DRIFT_LIMITS:
                        reference_luma = max(sequence["64"][plane]["meanLuma"], 1.0)
                        for key, relative, absolute in zip(("mae", "p95"), DRIFT_LIMITS[frame], DRIFT_LIMITS_CODES[frame]):
                            drift = broad["lumaDifferenceFromSettled"][key]
                            metric = f"{label} 32 px drift {key.upper()}"
                            check_upper(f"{metric} in display codes", drift, absolute)
                            check_upper(f"{metric} / settled mean luma", drift / reference_luma, relative)

                    if group == "light-edit" and frame in EDIT_COMPLETION_MINIMUM:
                        completion = broad["editCompletion"]
                        minimum = EDIT_COMPLETION_MINIMUM[frame]
                        if completion is None or completion < minimum:
                            value = "no measurable response" if completion is None else f"{completion:.3f}"
                            errors.append(f"{label} edit completion {value}, expected at least {minimum:.3f}")
                    if group == "light-edit" and frame == 64:
                        signal = broad["settledRgbDifferenceFromBeforeEdit"]["mae"]
                        if signal < MIN_EDIT_SIGNAL_CODES:
                            errors.append(f"{label} edit signal {signal:.2f} is below {MIN_EDIT_SIGNAL_CODES:.2f} codes")
    return errors


def print_summary(label, report):
    print(f"{label}  {report['directory']}")
    print("32 px tiles, f1/f4/f8/f64  |  luma curvature P95  |  luma drift MAE  |  mean luma")
    for method, mode in report["methods"].items():
        for group in ("diffuse", "light-edit"):
            for plane in PLANES:
                rows = [mode[group][str(frame)][plane] for frame in (1, 4, 8, 64)]
                curvature = [row["tiles"]["32"]["lumaCurvatureP95"] for row in rows]
                drift = [row["tiles"]["32"]["lumaDifferenceFromSettled"]["mae"] for row in rows]
                luma = [row["meanLuma"] for row in rows]
                values = ["/".join(f"{value:5.2f}" for value in series) for series in (curvature, drift, luma)]
                print(f"{method:4} {group:10} {plane:8}  {'  |  '.join(values)}")
        for group, sequence in mode["grain"].items():
            ceiling = sequence["1"]["ceiling"]
            values = "/".join(f"{ceiling['windows'][str(size)]['lumaResidualP95']:.2f}" for size in GRAIN_WINDOWS)
            print(f"{method:4} {group:10} ceiling f1 high-pass P95, 5/9/17 px  |  {values}")
        if "orbit" in mode:
            for plane, row in mode["orbit"]["moving"].items():
                rest = mode["orbit"]["settled"][plane]
                moving_p95 = row["windows"]["9"]["lumaResidualP95"]
                settled_p95 = rest["windows"]["9"]["lumaResidualP95"]
                drift = row["lumaDifferenceFromRest"]["mae"]
                print(f"{method:4} orbit      {plane:8}  9 px P95 moving/rest {moving_p95:.2f}/{settled_p95:.2f}"
                      f"  |  moving-to-rest luma MAE {drift:.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog="Dependencies: python -m pip install numpy pillow")
    parser.add_argument("capture_directory", type=Path, help="directory containing software-gi.json and its BMPs")
    parser.add_argument("--baseline", type=Path, help="compare another capture of the same Cornell fixture")
    parser.add_argument("--output", type=Path, help="write tile, high-frequency grain and orbit measurements as JSON")
    parser.add_argument("--check", action="store_true", help="enforce the fixed Cornell fixture's quality limits")
    args = parser.parse_args()
    if DEPENDENCY_ERROR:
        parser.error(f"{DEPENDENCY_ERROR}. Install the measurement dependencies with python -m pip install numpy pillow")

    try:
        candidate = Captures(args.capture_directory)
        candidate_normals = candidate.image("normals", 1, 3)
        baseline = Captures(args.baseline) if args.baseline else None
        if baseline:
            for field in COMPARABLE_SETTINGS:
                if candidate.metadata[field] != baseline.metadata[field]:
                    raise ValueError(f"Candidate and baseline differ in {field}")
            if not np.array_equal(candidate_normals, baseline.image("normals", 1, 3)):
                raise ValueError("Candidate and baseline have different G-buffer geometry or camera")
            for method in ("ddgi", "ssgi"):
                key = orbit_key(method)
                if candidate.has(key) and baseline.has(key):
                    if not np.array_equal(candidate.image(key, 1, 3), baseline.image(key, 1, 3)):
                        raise ValueError(f"Candidate and baseline have different {method} orbit geometry or camera")
        masks = plane_masks(candidate_normals)
        report = {"units": "8-bit display codes", "settledReferenceFrame": 64,
                  "candidate": measure(candidate, masks)}
        if baseline:
            report["baseline"] = measure(baseline, masks)
            print_summary("Baseline", report["baseline"])
        print_summary("Candidate", report["candidate"])
        errors = quality_errors(report["candidate"], report.get("baseline")) if args.check else []
        if args.check:
            report["checks"] = {"passed": not errors, "failures": errors}
            for error in errors:
                print(f"FAIL  {error}", file=sys.stderr)
            if not errors:
                print("PASS  Cornell software GI quality limits")
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (KeyError, OSError, ValueError) as error:
        print(f"Software GI measurement failed: {error}", file=sys.stderr)
        return 1
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())

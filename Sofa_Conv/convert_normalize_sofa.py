#!/usr/bin/env python3
"""
Convert and normalize SOFA HRTF files to the AT WaveSpace .txt format.
Supports single-file and batch (directory) processing.

Dependencies (install one):
    pip install sofar
    pip install pysofaconventions
    pip install netCDF4
"""

import sys
import os
import numpy as np
from pathlib import Path


def load_sofa_file(sofa_path):
    """
    Load a SOFA file and return (positions, ir_data, sample_rate, backend).
    Tries sofar, then pysofaconventions, then netCDF4.
    """
    # Backend 1: sofar
    try:
        import sofar as sf
        sofa = sf.read_sofa(sofa_path)
        return sofa.SourcePosition, sofa.Data_IR, int(sofa.Data_SamplingRate), "sofar"
    except (ImportError, Exception):
        pass

    # Backend 2: pysofaconventions
    try:
        import pysofaconventions as pysofa
        sofa = pysofa.SOFAFile(sofa_path, 'r')
        positions  = sofa.getVariableValue('SourcePosition')
        ir_data    = sofa.getDataIR()
        sample_rate = int(sofa.getSamplingRate()[0])
        sofa.close()
        return positions, ir_data, sample_rate, "pysofaconventions"
    except (ImportError, Exception):
        pass

    # Backend 3: netCDF4
    try:
        from netCDF4 import Dataset
        with Dataset(sofa_path, 'r') as nc:
            positions   = nc.variables['SourcePosition'][:]
            ir_data     = nc.variables['Data.IR'][:]
            sample_rate = int(nc.variables['Data.SamplingRate'][:][0])
        return positions, ir_data, sample_rate, "netCDF4"
    except (ImportError, Exception):
        pass

    return None, None, None, None


def detect_channel_swap(ir_left, ir_right, azimuths, elevations):
    """
    Detect L/R channel swap by checking peak energy at ~90° (should be louder on right)
    and ~270° (should be louder on left). Returns (should_swap, confidence).
    """
    ratio_90 = ratio_270 = None

    mask_90 = (np.abs(azimuths - 90.0) < 10) & (np.abs(elevations) < 20)
    if np.any(mask_90):
        idx = np.where(mask_90)[0][0]
        el = np.max(np.abs(ir_left[idx]))
        er = np.max(np.abs(ir_right[idx]))
        ratio_90 = er / el if el > 1e-6 else 1000.0

    mask_270 = (np.abs(azimuths - 270.0) < 10) & (np.abs(elevations) < 20)
    if np.any(mask_270):
        idx = np.where(mask_270)[0][0]
        el = np.max(np.abs(ir_left[idx]))
        er = np.max(np.abs(ir_right[idx]))
        ratio_270 = el / er if er > 1e-6 else 1000.0

    if ratio_90 is not None and ratio_270 is not None:
        if ratio_90 < 0.5 and ratio_270 < 0.5:
            return True, 0.9
        if ratio_90 > 2.0 and ratio_270 > 2.0:
            return False, 0.9

    return False, 0.0


def normalize_irs(ir_left, ir_right):
    """
    Peak-normalize both IRs to 0.95 if the maximum exceeds that threshold.
    Returns (ir_left, ir_right, scale_factor, was_normalized).
    """
    max_val = max(np.max(np.abs(ir_left)), np.max(np.abs(ir_right)))
    target  = 0.95

    if max_val > target:
        scale = target / max_val
        return ir_left * scale, ir_right * scale, scale, True

    return ir_left, ir_right, 1.0, False


def convert_and_normalize_sofa(sofa_file, output_file=None, verbose=True):
    """
    Convert a SOFA file to the AT WaveSpace .txt format and normalize the IRs.
    Returns (success: bool, info: dict | error_msg: str).

    Output format (one line per measurement position):
        HEADER <sample_rate> <ir_length>
        <az> <el> <dist> <ir_left[0]> ... <ir_left[N-1]> <ir_right[0]> ... <ir_right[N-1]>
    """
    if verbose:
        print(f"Processing: {os.path.basename(sofa_file)}")

    if output_file is None:
        output_file = str(Path(sofa_file).with_suffix('.txt'))

    positions, ir_data, sample_rate, backend = load_sofa_file(sofa_file)

    if positions is None:
        msg = "Could not load SOFA file. Install sofar, pysofaconventions, or netCDF4."
        if verbose:
            print(f"  ERROR: {msg}")
        return False, msg

    if verbose:
        print(f"  Loaded with {backend}")

    try:
        positions = np.array(positions)
        ir_data   = np.array(ir_data)

        if ir_data.ndim != 3:
            msg = f"Unexpected IR data shape: {ir_data.shape}"
            if verbose:
                print(f"  ERROR: {msg}")
            return False, msg

        num_positions = positions.shape[0]
        ir_length     = ir_data.shape[2]

        ir_left  = ir_data[:, 0, :]
        ir_right = ir_data[:, 1, :] if ir_data.shape[1] >= 2 else ir_data[:, 0, :]

        azimuths   = positions[:, 0]
        elevations = positions[:, 1]
        distances  = positions[:, 2] if positions.shape[1] >= 3 else np.ones(num_positions)

        if verbose:
            print(f"  {num_positions} positions, {sample_rate} Hz, {ir_length} samples/IR")

        # L/R swap detection
        should_swap, confidence = detect_channel_swap(ir_left, ir_right, azimuths, elevations)
        was_swapped = should_swap and confidence > 0.5
        if was_swapped:
            ir_left, ir_right = ir_right, ir_left
            if verbose:
                print("  WARNING: L/R channels were swapped — corrected automatically")

        # Normalization
        ir_left, ir_right, scale_factor, was_normalized = normalize_irs(ir_left, ir_right)
        if verbose:
            if was_normalized:
                print(f"  Normalized (scale factor: {scale_factor:.6f})")
            else:
                print("  No normalization needed")

        # Write output
        if verbose:
            print(f"  Writing: {os.path.basename(output_file)}")

        with open(output_file, 'w') as f:
            f.write(f"HEADER {sample_rate} {ir_length}\n")
            for i in range(num_positions):
                f.write(f"{azimuths[i]:.2f} {elevations[i]:.2f} {distances[i]:.3f}")
                for v in ir_left[i]:
                    f.write(f" {v:.6f}")
                for v in ir_right[i]:
                    f.write(f" {v:.6f}")
                f.write("\n")

        info = {
            'positions':      num_positions,
            'sample_rate':    sample_rate,
            'ir_length':      ir_length,
            'was_normalized': was_normalized,
            'was_swapped':    was_swapped,
            'scale_factor':   scale_factor,
            'output_file':    output_file,
        }

        if verbose:
            print("  Done.")

        return True, info

    except Exception as e:
        if verbose:
            print(f"  ERROR: {e}")
        return False, str(e)


def process_batch(input_path, verbose=True):
    """
    Convert all .sofa files found recursively under input_path.
    Returns (success_count, fail_count, results).
    """
    input_path = Path(input_path)

    if not input_path.is_dir():
        print(f"ERROR: '{input_path}' is not a directory")
        return 0, 0, []

    sofa_files = list(input_path.rglob("*.sofa"))

    if not sofa_files:
        print(f"No .sofa files found in '{input_path}'")
        return 0, 0, []

    print(f"\n{len(sofa_files)} .sofa file(s) found")
    print("-" * 60)

    results = []
    success_count = fail_count = 0

    for i, sofa_file in enumerate(sofa_files, 1):
        print(f"\n[{i}/{len(sofa_files)}]")
        output_file = sofa_file.with_suffix('.txt')
        success, info = convert_and_normalize_sofa(str(sofa_file), str(output_file), verbose)
        if success:
            success_count += 1
        else:
            fail_count += 1
        results.append({'input': str(sofa_file), 'success': success, 'info': info})

    return success_count, fail_count, results


def print_summary(success_count, fail_count, results):
    """Print a batch conversion summary."""
    print("\n" + "-" * 60)
    print(f"SUMMARY  OK: {success_count}  FAILED: {fail_count}  TOTAL: {success_count + fail_count}")

    for r in results:
        if r['success']:
            info = r['info']
            flags = []
            if info['was_normalized']:
                flags.append("normalized")
            if info['was_swapped']:
                flags.append("channels corrected")
            tag = f"  [{', '.join(flags)}]" if flags else ""
            print(f"  OK  {os.path.basename(r['info']['output_file'])}"
                  f"  {info['positions']} pos, {info['sample_rate']} Hz, {info['ir_length']} smp{tag}")
        else:
            print(f"  FAIL  {os.path.basename(r['input'])}: {r['info']}")


def main():
    print("SOFA -> TXT  |  AT WaveSpace HRTF converter")
    print("-" * 60)

    if len(sys.argv) < 2:
        print("Usage:")
        print("  Single file:  python3 convert_normalize_sofa.py input.sofa [output.txt]")
        print("  Batch:        python3 convert_normalize_sofa.py /path/to/folder/")
        sys.exit(1)

    input_path = sys.argv[1]

    if not os.path.exists(input_path):
        print(f"ERROR: '{input_path}' does not exist")
        sys.exit(1)

    # Batch mode
    if os.path.isdir(input_path):
        success_count, fail_count, results = process_batch(input_path)
        print_summary(success_count, fail_count, results)
        sys.exit(0 if fail_count == 0 else 1)

    # Single file
    if not input_path.endswith('.sofa'):
        print("ERROR: input file must have a .sofa extension")
        sys.exit(1)

    output_file = sys.argv[2] if len(sys.argv) >= 3 else None

    print()
    success, info = convert_and_normalize_sofa(input_path, output_file)

    print("\n" + "-" * 60)
    if success:
        print(f"Output:      {os.path.basename(info['output_file'])}")
        print(f"Positions:   {info['positions']}")
        print(f"Sample rate: {info['sample_rate']} Hz")
        print(f"IR length:   {info['ir_length']} samples")
        print(f"Normalized:  {'yes (factor: ' + str(round(info['scale_factor'],6)) + ')' if info['was_normalized'] else 'no'}")
        print(f"Ch. swapped: {'yes (corrected)' if info['was_swapped'] else 'no'}")
        sys.exit(0)
    else:
        print(f"FAILED: {info}")
        print("\nInstall a SOFA backend:")
        print("  pip install sofar")
        print("  pip install pysofaconventions")
        print("  pip install netCDF4")
        sys.exit(1)


if __name__ == "__main__":
    main()

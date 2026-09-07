# SPDX-License-Identifier: GPL-3.0-only
"""Encode rendered SBAS map PNGs into a previewable HEVC/MP4 video."""

import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import click
from tqdm import tqdm

from .gpst import label as gpst_label


def png_dimensions(path):
    with Path(path).open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Invalid PNG header: {path}")
    return struct.unpack(">II", header[16:24])


def load_images(manifest_path):
    manifest_path = Path(manifest_path)
    root = manifest_path.parent.resolve()
    document = json.loads(manifest_path.read_text())
    records = document.get("images")
    if not isinstance(records, list) or not records:
        raise ValueError("Image manifest has no images")

    images = []
    dimensions = None
    progress = tqdm(records, desc="Read PNG headers", unit="image")
    for record in progress:
        if "hour_utc" in record:
            raise ValueError("Old UTC image manifests are not supported; rerender with GPST tools")
        relative = record.get("path")
        if not isinstance(relative, str):
            raise ValueError("Image manifest contains an invalid path")
        path = (root / relative).resolve()
        if not path.is_relative_to(root):
            raise ValueError(f"Image path escapes the manifest directory: {relative}")
        if path.suffix.lower() != ".png" or not path.is_file():
            raise ValueError(f"Image is missing or is not a PNG: {relative}")
        size = png_dimensions(path)
        if dimensions is None:
            dimensions = size
        elif size != dimensions:
            raise ValueError(f"Image dimensions changed at {relative}: {size} != {dimensions}")
        images.append((path, record))
    return images, dimensions


def video_filter(width, height):
    if width % 2 or height % 2:
        return "pad=ceil(iw/2)*2:ceil(ih/2)*2,format=nv12,hwupload", True
    return "format=nv12,hwupload", False


def encode_command(ffmpeg, device, pattern, output, width, height, fps, quality, title, frame_count):
    filter_graph, padded = video_filter(width, height)
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostats",
        "-progress",
        "pipe:1",
        "-init_hw_device",
        f"vulkan=video:{device}",
        "-filter_hw_device",
        "video",
        "-framerate",
        str(fps),
        "-start_number",
        "0",
        "-i",
        str(pattern),
        "-vf",
        filter_graph,
        "-c:v",
        "hevc_vulkan",
        "-profile:v",
        "main",
        "-rc_mode",
        "cqp",
        "-qp",
        str(quality),
        "-r",
        str(fps),
        "-fps_mode",
        "cfr",
        "-tag:v",
        "hvc1",
        "-movflags",
        "+faststart",
        "-map_metadata",
        "-1",
        "-metadata",
        f"title={title}",
        "-metadata",
        f"comment={frame_count} manifest-ordered hourly maps at {fps} fps; gaps are not synthesized",
        "-an",
        "-y",
        str(output),
    ]
    return command, filter_graph, padded


def run_with_progress(command, frame_count):
    with tempfile.TemporaryFile(mode="w+t") as errors:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, text=True)
        last_frame = 0
        with tqdm(total=frame_count, desc="Encode HEVC", unit="frame") as progress:
            for line in process.stdout:
                key, separator, value = line.rstrip().partition("=")
                if separator and key == "frame":
                    frame = min(int(value), frame_count)
                    progress.update(max(0, frame - last_frame))
                    last_frame = frame
            return_code = process.wait()
            progress.update(max(0, frame_count - last_frame) if return_code == 0 else 0)
        if return_code:
            errors.seek(0)
            detail = errors.read().strip()
            raise RuntimeError(f"FFmpeg encoding failed with exit status {return_code}: {detail}")


def probe_video(ffprobe, path):
    command = [
        ffprobe,
        "-v",
        "error",
        "-count_packets",
        "-show_entries",
        "stream=codec_name,codec_tag_string,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_packets:format=duration,size,bit_rate",
        "-of",
        "json",
        str(path),
    ]
    return json.loads(subprocess.run(command, check=True, capture_output=True, text=True).stdout)


def validate_probe(probe, frame_count, width, height, fps, padded):
    streams = probe.get("streams", [])
    if len(streams) != 1:
        raise ValueError("Encoded file does not contain exactly one video stream")
    stream = streams[0]
    expected_width = width + width % 2 if padded else width
    expected_height = height + height % 2 if padded else height
    expected_rate = f"{fps}/1"
    expected = {
        "codec_name": "hevc",
        "codec_tag_string": "hvc1",
        "width": expected_width,
        "height": expected_height,
        "r_frame_rate": expected_rate,
        "avg_frame_rate": expected_rate,
        "nb_read_packets": str(frame_count),
    }
    for key, value in expected.items():
        if stream.get(key) != value:
            raise ValueError(f"Encoded stream has unexpected {key}: {stream.get(key)!r} != {value!r}")
    expected_duration = frame_count / fps
    duration = float(probe["format"]["duration"])
    if abs(duration - expected_duration) > 1 / fps:
        raise ValueError(f"Encoded duration is unexpected: {duration} != {expected_duration}")


def verify_decode(ffmpeg, path):
    result = subprocess.run(
        [ffmpeg, "-v", "error", "-i", str(path), "-map", "0:v:0", "-f", "null", "-"],
        check=False,
        capture_output=True,
        text=True,
    )
    errors = result.stderr.strip()
    if result.returncode or errors:
        detail = errors or f"exit status {result.returncode}"
        raise ValueError(f"Full video decode reported an error: {detail}")


def atomic_jsonlines(path, images, fps, overwrite):
    mode = "w" if overwrite else "x"
    temporary = path.with_name(f".{path.name}.partial")
    if temporary.exists():
        temporary.unlink()
    with temporary.open(mode) as stream:
        for number, (_, record) in enumerate(images):
            row = {
                "frame_index": number,
                "video_time_seconds": number / fps,
                "hour_gpst": record.get("hour_gpst"),
                "png": record["path"],
            }
            if row["hour_gpst"] is not None:
                row["hour_label_gpst"] = gpst_label(row["hour_gpst"])
            stream.write(json.dumps(row, separators=(",", ":"), sort_keys=True) + "\n")
    os.replace(temporary, path)


@click.command()
@click.option(
    "--images-manifest",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    required=True,
    help="Renderer images.json whose order defines the video frames.",
)
@click.option("--output", type=click.Path(dir_okay=False, path_type=Path), required=True, help="Destination HEVC MP4 file.")
@click.option("--device", default="0", show_default=True, help="FFmpeg Vulkan physical-device selector.")
@click.option("--fps", type=click.IntRange(min=1), default=5, show_default=True)
@click.option("--quality", type=click.IntRange(min=0, max=51), default=24, show_default=True, help="Vulkan Video CQP value.")
@click.option("--title", default="SBAS hourly mean VTEC", show_default=True)
@click.option(
    "--verify-output/--no-verify-output", default=False, show_default=True, help="Decode the complete video after encoding."
)
@click.option("--overwrite", is_flag=True, help="Replace an existing video and sidecars after the new video verifies.")
def cli(images_manifest, output, device, fps, quality, title, verify_output, overwrite):
    """Encode manifest-ordered SBAS map PNGs with Vulkan Video HEVC."""
    output = output.resolve()
    if output.suffix.lower() != ".mp4":
        raise click.UsageError("--output must use the .mp4 extension")
    output.parent.mkdir(parents=True, exist_ok=True)
    frames_path = output.with_suffix(".frames.jsonl")
    existing = [path for path in (output, frames_path) if path.exists()]
    if existing and not overwrite:
        raise click.ClickException(f"Output already exists: {existing[0]}")

    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise click.ClickException("ffmpeg and ffprobe must both be available")

    try:
        images, (width, height) = load_images(images_manifest)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise click.ClickException(str(error)) from error

    partial = output.with_name(f".{output.stem}.partial.mp4")
    if partial.exists():
        partial.unlink()
    command = None
    try:
        with tempfile.TemporaryDirectory(prefix="neognss-video-") as temporary:
            sequence = Path(temporary)
            for number, (source, _) in enumerate(images):
                (sequence / f"frame-{number:08d}.png").symlink_to(source)
            pattern = sequence / "frame-%08d.png"
            command, filter_graph, padded = encode_command(
                ffmpeg, device, pattern, partial, width, height, fps, quality, title, len(images)
            )
            run_with_progress(command, len(images))

        probe = probe_video(ffprobe, partial)
        validate_probe(probe, len(images), width, height, fps, padded)
        if verify_output:
            verify_decode(ffmpeg, partial)
        atomic_jsonlines(frames_path, images, fps, overwrite)
        os.replace(partial, output)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        if partial.exists():
            partial.unlink()
        raise click.ClickException(str(error)) from error

    click.echo(
        json.dumps(
            {
                "status": "complete",
                "video": str(output),
                "frames": len(images),
                "duration_seconds": len(images) / fps,
                "width": width + width % 2 if padded else width,
                "height": height + height % 2 if padded else height,
                "padded": padded,
                "frame_index": str(frames_path),
            },
            separators=(",", ":"),
        )
    )


if __name__ == "__main__":
    cli()

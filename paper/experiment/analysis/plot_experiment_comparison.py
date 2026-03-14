#!/usr/bin/env python3
"""
Generate publication-quality comparison plots for the experiment (CATS vs Baseline TCP).

Inputs:
  - baseline/priority_completion.tsv
  - cats/priority_completion.tsv

Outputs:
  - completion_comparison.png
  - throughput_comparison.png
"""

from __future__ import annotations

import argparse
from io import StringIO
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

PRIORITY_SIZES = {
    0: 8000,
    1: 25000,
    2: 40000,
    3: 60000,
    4: 150000,
}

PRIORITY_LABELS = {
    0: "P0\nCritical HTML/CSS",
    1: "P1\nCSS Framework",
    2: "P2\nApplication JS",
    3: "P3\nImages/Media",
    4: "P4\nAnalytics/Tracking",
}

COLORS = {
    "baseline": "#2F5597",
    "cats": "#C55A11",
    "grid": "#D9D9D9",
    "text": "#222222",
}


def configure_style() -> None:
    plt.style.use("seaborn-v0_8-whitegrid")
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 11,
            "axes.labelsize": 13,
            "axes.titlesize": 15,
            "xtick.labelsize": 10,
            "ytick.labelsize": 10,
            "legend.fontsize": 11,
            "axes.edgecolor": COLORS["text"],
            "axes.linewidth": 1.0,
            "figure.facecolor": "white",
            "axes.facecolor": "white",
        }
    )


def load_priority_completion(tsv_path: Path) -> pd.DataFrame:
    lines: list[str] = []
    for raw in tsv_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            lines.append(line)

    if not lines:
        raise ValueError(f"No data rows found in {tsv_path}")

    df = pd.read_csv(
        StringIO("\n".join(lines)),
        sep="\t",
        header=None,
        names=["SimTime(s)", "PageID", "Priority", "CompletionTime(ms)", "Event"],
    )

    for col in ["Priority", "CompletionTime(ms)"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    df = df.dropna(subset=["Priority", "CompletionTime(ms)"]).copy()
    df["Priority"] = df["Priority"].astype(int)
    df = df[df["Event"] == "GROUP_COMPLETED"]

    return df


def completion_by_priority(df: pd.DataFrame) -> pd.Series:
    # One row per priority is expected; min() is robust if duplicates appear.
    out = df.groupby("Priority")["CompletionTime(ms)"].min()
    for p in PRIORITY_SIZES:
        if p not in out.index:
            out.loc[p] = np.nan
    return out.sort_index()


def throughput_mbps(priority: int, completion_ms: float) -> float:
    if completion_ms <= 0 or np.isnan(completion_ms):
        return np.nan
    bits = PRIORITY_SIZES[priority] * 8.0
    return bits / (completion_ms / 1000.0) / 1_000_000.0


def annotate_bars(ax: plt.Axes, bars, fmt: str, ymax: float) -> None:
    offset = ymax * 0.025
    for bar in bars:
        h = bar.get_height()
        if np.isnan(h) or h <= 0:
            continue
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            h + offset,
            format(h, fmt),
            ha="center",
            va="bottom",
            fontsize=9,
            color=COLORS["text"],
            bbox={"boxstyle": "round,pad=0.22", "facecolor": "white", "edgecolor": "none", "alpha": 0.85},
            zorder=5,
        )


def plot_completion(baseline: pd.Series, cats: pd.Series, out_path: Path) -> None:
    x = np.arange(len(PRIORITY_SIZES))
    width = 0.38

    fig, ax = plt.subplots(figsize=(12.5, 7.2), dpi=150)

    bars_baseline = ax.bar(
        x - width / 2,
        baseline.values,
        width=width,
        color=COLORS["baseline"],
        label="Baseline TCP",
        edgecolor="white",
        linewidth=1.1,
        alpha=0.9,
        zorder=3,
    )
    bars_cats = ax.bar(
        x + width / 2,
        cats.values,
        width=width,
        color=COLORS["cats"],
        label="CATS",
        edgecolor="white",
        linewidth=1.1,
        alpha=0.9,
        zorder=3,
    )

    ymax = float(np.nanmax(np.concatenate([baseline.values, cats.values])))
    ax.set_ylim(0, ymax * 1.16)
    ax.set_axisbelow(True)
    ax.grid(axis="y", color=COLORS["grid"], linewidth=0.8, alpha=0.7)

    ax.set_xlabel("Priority Group")
    ax.set_ylabel("Completion Time (ms)")
    ax.set_title("Comparison of Priority Group Completion Times", pad=14, color=COLORS["text"], weight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels([PRIORITY_LABELS[p] for p in sorted(PRIORITY_LABELS)])

    annotate_bars(ax, bars_baseline, ".0f", ymax)
    annotate_bars(ax, bars_cats, ".0f", ymax)

    # Keep legend outside so bars are never occluded.
    leg = ax.legend(loc="center left", bbox_to_anchor=(1.01, 0.5), frameon=True)
    leg.get_frame().set_edgecolor(COLORS["grid"])
    leg.get_frame().set_linewidth(1.0)

    fig.tight_layout()
    fig.subplots_adjust(right=0.82)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def plot_throughput(baseline: pd.Series, cats: pd.Series, out_path: Path) -> None:
    baseline_tp = pd.Series(
        {p: throughput_mbps(p, baseline.loc[p]) for p in sorted(PRIORITY_SIZES)},
        name="Baseline TCP",
    )
    cats_tp = pd.Series(
        {p: throughput_mbps(p, cats.loc[p]) for p in sorted(PRIORITY_SIZES)},
        name="CATS",
    )

    x = np.arange(len(PRIORITY_SIZES))
    width = 0.38

    fig, ax = plt.subplots(figsize=(12.5, 7.2), dpi=150)

    bars_baseline = ax.bar(
        x - width / 2,
        baseline_tp.values,
        width=width,
        color=COLORS["baseline"],
        label="Baseline TCP",
        edgecolor="white",
        linewidth=1.1,
        alpha=0.9,
        zorder=3,
    )
    bars_cats = ax.bar(
        x + width / 2,
        cats_tp.values,
        width=width,
        color=COLORS["cats"],
        label="CATS",
        edgecolor="white",
        linewidth=1.1,
        alpha=0.9,
        zorder=3,
    )

    ymax = float(np.nanmax(np.concatenate([baseline_tp.values, cats_tp.values])))
    ax.set_ylim(0, ymax * 1.16)
    ax.set_axisbelow(True)
    ax.grid(axis="y", color=COLORS["grid"], linewidth=0.8, alpha=0.7)

    ax.set_xlabel("Priority Group")
    ax.set_ylabel("Effective Throughput (Mbps)")
    ax.set_title("Comparison of Effective Throughput", pad=14, color=COLORS["text"], weight="bold")

    xlabels = [
        f"{PRIORITY_LABELS[p]}\n({PRIORITY_SIZES[p] / 1000:.0f} KB)"
        for p in sorted(PRIORITY_SIZES)
    ]
    ax.set_xticks(x)
    ax.set_xticklabels(xlabels)

    annotate_bars(ax, bars_baseline, ".2f", ymax)
    annotate_bars(ax, bars_cats, ".2f", ymax)

    leg = ax.legend(loc="center left", bbox_to_anchor=(1.01, 0.5), frameon=True)
    leg.get_frame().set_edgecolor(COLORS["grid"])
    leg.get_frame().set_linewidth(1.0)

    fig.tight_layout()
    fig.subplots_adjust(right=0.82)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate experiment comparison plots.")
    parser.add_argument("--baseline", required=True, type=Path, help="Path to baseline priority_completion.tsv")
    parser.add_argument("--cats", required=True, type=Path, help="Path to CATS priority_completion.tsv")
    parser.add_argument("--outdir", required=True, type=Path, help="Output directory for PNG plots")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    configure_style()

    args.outdir.mkdir(parents=True, exist_ok=True)

    baseline_df = load_priority_completion(args.baseline)
    cats_df = load_priority_completion(args.cats)

    baseline = completion_by_priority(baseline_df)
    cats = completion_by_priority(cats_df)

    plot_completion(baseline, cats, args.outdir / "completion_comparison.png")
    plot_throughput(baseline, cats, args.outdir / "throughput_comparison.png")

    print("Generated:")
    print(f"  - {args.outdir / 'completion_comparison.png'}")
    print(f"  - {args.outdir / 'throughput_comparison.png'}")


if __name__ == "__main__":
    main()

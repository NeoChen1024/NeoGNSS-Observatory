# SPDX-License-Identifier: GPL-3.0-only
"""Ordered, bounded read-ahead for batch iterators."""

from concurrent.futures import ThreadPoolExecutor


def prefetched(source, *, thread_name):
    """One iterator owner, one prefetched item, and joined shutdown on failure."""
    source = iter(source)
    end = object()
    with ThreadPoolExecutor(max_workers=1, thread_name_prefix=thread_name) as pool:
        future = pool.submit(next, source, end)
        try:
            while (batch := future.result()) is not end:
                future = pool.submit(next, source, end)
                yield batch
        finally:
            future.cancel()
            pool.shutdown(wait=True, cancel_futures=True)
            if close := getattr(source, "close", None):
                close()

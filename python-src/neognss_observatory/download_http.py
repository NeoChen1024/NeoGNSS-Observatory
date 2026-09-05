# SPDX-License-Identifier: GPL-3.0-only
"""Bounded HTTPS access with explicit Earthdata credential scoping."""

import email.utils
import http.cookiejar
import netrc
import os
import stat
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urljoin, urlsplit, urlunsplit

import requests

from .download_common import AuthError, DownloadError

ARCHIVE = "https://cddis.nasa.gov/archive/"
URS = "https://urs.earthdata.nasa.gov"
DEFAULT_NETWORK = {"workers": 2, "requests_per_second": 2.0, "connect_timeout": 15, "read_timeout": 120, "retries": 5}


def retry_delay(value, attempt):
    try:
        return max(0, float(value))
    except (ValueError, TypeError):
        try:
            return max(0, email.utils.parsedate_to_datetime(value).timestamp() - time.time())
        except (ValueError, TypeError, OverflowError):
            return min(60, 2**attempt)


class HTTP:
    def __init__(self, netrc_path, settings, cookie_path=None):
        path = Path(netrc_path).expanduser()
        try:
            info = path.stat()
            if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
                raise AuthError("The netrc file must belong to you and have mode 0600 or stricter")
            credentials = netrc.netrc(path).authenticators("urs.earthdata.nasa.gov")
        except (OSError, netrc.NetrcParseError) as exc:
            raise AuthError("Cannot read a valid netrc file") from exc
        if not credentials or not credentials[0] or not credentials[2]:
            raise AuthError("The netrc file needs an urs.earthdata.nasa.gov login and password")
        self.auth = (credentials[0], credentials[2])
        self.settings = settings
        cache = Path(os.environ.get("XDG_CACHE_HOME", "~/.cache")).expanduser()
        self.cookie_path = Path(cookie_path).expanduser() if cookie_path else cache / "neognss-observatory/cookies.txt"
        self.cookies = http.cookiejar.MozillaCookieJar()
        if self.cookie_path.exists():
            if stat.S_IMODE(self.cookie_path.stat().st_mode) & 0o077:
                raise AuthError("The cookie cache must have mode 0600 or stricter")
            try:
                self.cookies.load(str(self.cookie_path), ignore_discard=True)
            except (OSError, http.cookiejar.LoadError):
                pass
        self.lock = threading.Lock()
        self.next_request = 0.0
        self.stopped = threading.Event()

    def throttle(self):
        while True:
            if self.stopped.is_set():
                raise DownloadError("Download interrupted")
            with self.lock:
                wait = self.next_request - time.monotonic()
                if wait <= 0:
                    self.next_request = time.monotonic() + 1 / self.settings["requests_per_second"]
                    return
            if self.stopped.wait(wait):
                raise DownloadError("Download interrupted")

    def backoff(self, value, attempt):
        delay = retry_delay(value, attempt)
        with self.lock:
            self.next_request = max(self.next_request, time.monotonic() + delay)
        if self.stopped.wait(delay):
            raise DownloadError("Download interrupted")

    def request(self, url, headers=None):
        """Return a streaming response. Caller must close it; redirects are host-checked."""
        for attempt in range(self.settings["retries"] + 1):
            session = requests.Session()
            session.trust_env = False  # No implicit netrc lookup or cross-host credentials.
            with self.lock:
                session.cookies.update(self.cookies)
            current = url
            try:
                for _ in range(12):
                    parts = urlsplit(current)
                    origin = f"{parts.scheme}://{parts.netloc}"
                    if origin not in ("https://cddis.nasa.gov", "https://files.igs.org", URS) or parts.username or parts.password:
                        raise DownloadError(f"Refusing unapproved redirect origin: {parts.scheme}://{parts.hostname}")
                    self.throttle()
                    response = session.get(
                        current,
                        headers={"Accept-Encoding": "identity", **(headers or {})} if origin != URS else {},
                        auth=self.auth if origin == URS else None,
                        timeout=(self.settings["connect_timeout"], self.settings["read_timeout"]),
                        allow_redirects=False,
                        stream=True,
                    )
                    with self.lock:
                        for cookie in session.cookies:
                            self.cookies.set_cookie(cookie)
                    if response.status_code in (301, 302, 303, 307, 308):
                        location = response.headers.get("Location")
                        response.close()
                        if not location:
                            raise DownloadError("Redirect without a Location header")
                        current = urljoin(current, location)
                        redirected = urlsplit(current)
                        # CDDIS proxyauth currently returns an HTTP archive URL after
                        # successful login. Upgrade locally; never send a plaintext request.
                        if (
                            redirected.scheme == "http"
                            and redirected.netloc == "cddis.nasa.gov"
                            and redirected.path.startswith("/archive/")
                        ):
                            current = urlunsplit(redirected._replace(scheme="https"))
                        continue
                    if response.status_code in (401, 403) or origin == URS:
                        response.close()
                        self.stopped.set()
                        raise AuthError("Earthdata authentication/authorization failed; check netrc and CDDIS approval")
                    if response.status_code == 429 or 500 <= response.status_code < 600:
                        delay = response.headers.get("Retry-After")
                        response.close()
                        if attempt == self.settings["retries"]:
                            raise DownloadError("HTTP retry limit exhausted")
                        self.backoff(delay, attempt)
                        break
                    # Session.close does not close this active response stream.
                    return response
                else:
                    self.stopped.set()
                    raise AuthError("Too many authentication redirects; check CDDIS approval or stale cookies")
            except requests.RequestException as exc:
                if attempt == self.settings["retries"]:
                    raise DownloadError("HTTPS connection failed after retries") from exc
                self.backoff(None, attempt)
            finally:
                session.close()
        raise DownloadError("HTTP retry limit exhausted")

    def text(self, url):
        for attempt in range(self.settings["retries"] + 1):
            try:
                return self._read_text(url)
            except (requests.RequestException, UnicodeError) as exc:
                if attempt == self.settings["retries"]:
                    # Exclude redirect queries and exception text, which can contain auth state.
                    parts = urlsplit(url)
                    location = f"{parts.hostname}{parts.path}"
                    raise DownloadError(f"Directory body retries exhausted at {location} ({type(exc).__name__})") from exc
                self.backoff(None, attempt)

    def _read_text(self, url):
        with self.request(url) as response:
            if response.status_code == 404:
                return None
            if response.status_code != 200:
                raise DownloadError(f"Unexpected HTTP status {response.status_code} during inventory")
            # Directory pages only; never use this for the giant checksum snapshot.
            blocks, size = [], 0
            for block in response.iter_content(65536):
                size += len(block)
                if size > 32 * 1024 * 1024:
                    raise DownloadError("Directory response exceeded 32 MiB")
                blocks.append(block)
            text = b"".join(blocks).decode("utf-8")
            if 'name="password"' in text.lower() or "earthdata login" in text.lower() and "<form" in text.lower():
                raise AuthError("Received an Earthdata login page instead of archive data")
            return text

    def close(self):
        self.cookie_path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(dir=self.cookie_path.parent)
        os.close(fd)
        try:
            self.cookies.save(temporary, ignore_discard=True, ignore_expires=False)
            os.chmod(temporary, 0o600)
            os.replace(temporary, self.cookie_path)
        finally:
            Path(temporary).unlink(missing_ok=True)

"""启动 Porism 桌面示例：拉起本地后端 + 用浏览器应用窗口打开前端。

前后端分离：
  - 后端 server.py —— 本地 HTTP API，负责调用 porism 求值内核
  - 前端 web/     —— GUI 页面，只通过 fetch 调用后端，不直接执行 porism

运行：python main.py
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import threading
import webbrowser
from pathlib import Path

# 允许作为脚本直接运行（python examples/rule-engine/main.py）
sys.path.insert(0, str(Path(__file__).resolve().parent))
from server import create_server  # noqa: E402


def open_app_window(url: str) -> bool:
    """用 Chromium 系浏览器的应用窗口模式打开（无地址栏，接近原生桌面应用）。"""
    extra_paths = [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ]
    found = []
    for name in ("msedge", "chrome", "chromium", "chromium-browser"):
        p = shutil.which(name)
        if p:
            found.append(p)
    found += [p for p in extra_paths if Path(p).is_file()]

    for browser in found:
        try:
            subprocess.Popen(
                [browser, f"--app={url}", "--window-size=470,560"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        except OSError:
            continue
    return False


def main() -> int:
    srv = create_server()
    host, port = srv.server_address[:2]
    url = f"http://{host}:{port}/"

    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    print(f"Porism 桌面示例已启动: {url}")
    print("关闭应用窗口或按 Ctrl+C 退出。")

    if not open_app_window(url):
        webbrowser.open(url)

    try:
        thread.join()
    except KeyboardInterrupt:
        print("\n正在关闭后端…")
    finally:
        srv.shutdown()
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

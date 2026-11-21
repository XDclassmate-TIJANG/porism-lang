"""Porism 规则引擎示例的后端：把求值内核包装成本地 HTTP API。

后端只负责三件事：提供静态前端文件、提供规则文件、校验并调用 porism 求值。
它不含任何界面代码；界面在 web/ 目录，通过 fetch 调用这里的接口。
所有外部输入（规则、参数、请求体）均设界限并校验，非法数据返回结构化诊断。
"""

from __future__ import annotations

import json
import math
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent
WEB = ROOT / "web"
RULE_FILE = ROOT / "rules.prsm"   # 业务规则配置文件，可编辑后即时生效

# ---- 界限常量：贴合本示例规模（一份十几行规则、3 个订单参数） ----
MAX_RULE_BYTES = 8 * 1024       # 规则文件最大 8 KiB
MAX_REQUEST_BYTES = 8 * 1024    # 单个请求体最大 8 KiB
MAX_CODE_CHARS = 4000           # 规则源码最大 4000 字符
MAX_PARAM_VALUE = 64            # 单个参数值最大长度
MAX_PARAMS = 16                 # 参数键数上限（本示例仅需 3 个）
MAX_NUMBER = 1e9                # 订单金额上限（十亿），超出视为非法输入

# 业务参数约束：订单数据的合法范围与白名单
PARAM_RULES = {
    "member": {"kind": "enum", "values": {"VIP", "PLUS", "NORMAL"}},
    "amount": {"kind": "number", "min": 0, "max": MAX_NUMBER},
    "coupon": {"kind": "number", "min": 0, "max": MAX_NUMBER},
}


def find_porism() -> Path:
    """优先用示例随附的二进制，其次用仓库正式构建产物。"""
    candidates = [
        ROOT / "bin" / ("porism.exe" if sys.platform == "win32" else "porism"),
        ROOT.parent.parent / "core" / "dist" / "windows-x86_64" / "bin" / "porism.exe",
    ]
    for p in candidates:
        if p.is_file():
            return p
    raise FileNotFoundError(
        "找不到 porism 可执行文件，请放到 examples/rule-engine/bin/ 或先执行 core/run.sh build"
    )


PORISM = find_porism()

# 静态文件白名单：URL 路径 -> 磁盘文件，杜绝路径穿越
STATIC = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
}


def error_result(message: str, kind: str = "input") -> dict:
    """构造与 porism 结果对象同形的错误，便于前端统一渲染。"""
    return {
        "status": "error",
        "errors": [{"type": kind, "message": message}],
        "outputs": [],
        "exitCode": None,
    }


def validate_params(params: dict) -> str | None:
    """校验注入参数：类型、长度、数值范围、枚举白名单。返回错误信息或 None。"""
    if len(params) > MAX_PARAMS:
        return f"参数过多：最多 {MAX_PARAMS} 个，实际 {len(params)} 个"

    for key, value in params.items():
        if not isinstance(key, str) or not key or len(key) > 32:
            return f"参数名非法：{key!r}"
        if not isinstance(value, (str, int, float, bool)):
            return f"参数 ${key} 的类型非法：{type(value).__name__}"
        text = str(value)
        if len(text) > MAX_PARAM_VALUE:
            return f"参数 ${key} 的值过长：最多 {MAX_PARAM_VALUE} 字符"

        spec = PARAM_RULES.get(key)
        if not spec:
            continue
        if spec["kind"] == "enum":
            if text not in spec["values"]:
                allowed = ", ".join(sorted(spec["values"]))
                return f"参数 ${key} 取值非法：{text!r}，允许 {allowed}"
        elif spec["kind"] == "number":
            try:
                num = float(text)
            except ValueError:
                return f"参数 ${key} 不是合法数字：{text!r}"
            if not math.isfinite(num):
                return f"参数 ${key} 不是有限数值：{text!r}"
            if num < spec["min"] or num > spec["max"]:
                return f"参数 ${key} 超出范围 [{spec['min']}, {spec['max']}]：{text!r}"

    # 业务交叉约束：优惠券不得超过订单金额
    if "amount" in params and "coupon" in params:
        try:
            if float(str(params["coupon"])) > float(str(params["amount"])):
                return "优惠券金额不得超过订单金额"
        except ValueError:
            pass  # 单项校验已拦截，这里不重复报错
    return None


def call_porism(code: str, params: dict) -> dict:
    """校验并执行规则，返回统一的结果对象（不把异常抛给前端）。"""
    if not code or not code.strip():
        return error_result("规则不能为空")
    if len(code) > MAX_CODE_CHARS:
        return error_result(f"规则过长：最多 {MAX_CODE_CHARS} 字符，实际 {len(code)}")

    problem = validate_params(params)
    if problem:
        return error_result(problem)

    flat = {str(k): str(v) for k, v in params.items()}
    payload = json.dumps(flat, separators=(",", ":"), ensure_ascii=False)

    try:
        proc = subprocess.run(
            [str(PORISM), "--json", code, "-p", payload],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            timeout=5,
        )
    except subprocess.TimeoutExpired:
        return error_result("求值超时：规则执行超过 5 秒", kind="kernel")
    except OSError as exc:
        return error_result(f"无法启动 porism: {exc}", kind="kernel")

    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        data = error_result(
            (proc.stdout or proc.stderr or f"退出码 {proc.returncode}").strip(),
            kind="kernel",
        )
    if not isinstance(data, dict):
        data = error_result("porism 返回了无法识别的结果", kind="kernel")
    data["exitCode"] = proc.returncode
    return data


class Handler(BaseHTTPRequestHandler):
    server_version = "PorismRuleEngine/1.0"

    def log_message(self, fmt: str, *args) -> None:
        # 保持控制台干净，只打印 API 调用摘要
        if urlparse(self.path).path == "/api/eval":
            print(f"[api] {self.command} {self.path}")

    def _send(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, status: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/api/health":
            self._send_json(200, {"status": "ok", "porism": str(PORISM)})
            return
        if path == "/api/rule":
            try:
                size = RULE_FILE.stat().st_size if RULE_FILE.is_file() else 0
                if size > MAX_RULE_BYTES:
                    self._send_json(413, {"error": f"规则文件超过 {MAX_RULE_BYTES} 字节上限"})
                    return
                rule = RULE_FILE.read_text(encoding="utf-8") if size else ""
            except OSError as exc:
                self._send_json(500, {"error": f"读取规则文件失败: {exc}"})
                return
            self._send_json(200, {"rule": rule, "file": str(RULE_FILE)})
            return
        if path not in STATIC:
            self._send_json(404, {"error": "not found"})
            return
        name, content_type = STATIC[path]
        file = WEB / name
        if not file.is_file():
            self._send_json(500, {"error": f"缺少前端文件 {name}"})
            return
        self._send(200, file.read_bytes(), content_type)

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path == "/api/shutdown":
            self._send_json(200, {"status": "ok"})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        if path != "/api/eval":
            self._send_json(404, {"error": "not found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json(400, error_result("Content-Length 非法"))
            return
        if length <= 0 or length > MAX_REQUEST_BYTES:
            self._send_json(413, error_result(f"请求体大小须在 1–{MAX_REQUEST_BYTES} 字节之间"))
            return

        try:
            req = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(req, dict):
                raise ValueError("请求体必须是 JSON 对象")
            code = req.get("code", "")
            params = req.get("params", {})
            if not isinstance(code, str) or not isinstance(params, dict):
                raise ValueError("code 必须是字符串，params 必须是对象")
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            self._send_json(400, error_result(f"请求格式错误: {exc}"))
            return

        # 校验失败也用 200 + 结果对象返回，前端统一按 status 渲染
        self._send_json(200, call_porism(code, params))


def create_server(host: str = "127.0.0.1", port: int = 0) -> ThreadingHTTPServer:
    return ThreadingHTTPServer((host, port), Handler)


if __name__ == "__main__":
    srv = create_server()
    host, port = srv.server_address[:2]
    print(f"Porism 规则引擎已启动: http://{host}:{port}  (porism: {PORISM})")
    print("按 Ctrl+C 停止")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()

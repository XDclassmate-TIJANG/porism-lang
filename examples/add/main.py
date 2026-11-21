import json
import subprocess
from pathlib import Path

PORISM = Path(__file__).resolve().parent / "bin" / "porism.exe"

CODE = """Add(a, b) = a + b
Output(Add(num($a), num($b)))"""

def run(a="1", b="2"):
    payload = json.dumps({"a": a, "b": b}, separators=(",", ":"))

    result = subprocess.run(
        [str(PORISM), CODE, "-p", payload],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )

    if result.returncode != 0:
        raise RuntimeError(f"porism 退出码 {result.returncode}: {result.stderr}")

    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError:
        raise RuntimeError(
            f"输出不是合法 JSON:\nstdout={result.stdout!r}\nstderr={result.stderr!r}"
        )

    if data.get("status") != "ok" or data.get("errors"):
        raise RuntimeError(f"porism 报错: {data.get('errors')}")

    return data["outputs"]

if __name__ == "__main__":
    outputs = run()
    print(outputs)          # ['3']
    print(int(outputs[0]))  # 3
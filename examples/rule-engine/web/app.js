// 前端逻辑：只包含界面交互与 HTTP 请求，不接触 porism 可执行文件。
// 业务规则从后端 rules.prsm 加载，可在界面编辑后重新提交求值。

const el = (id) => document.getElementById(id);
const result = el("result");
const badge = el("badge");
const exit = el("exit");
const paybox = el("paybox");
const payable = el("payable");
const breakdown = el("breakdown");
const errors = el("errors");
const raw = el("raw");
const conn = el("conn");
const runBtn = el("run");

// 规则约定的 4 个输出，顺序与 rules.prsm 末尾的 Output 对应
const FIELDS = ["折后小计", "满减金额", "会员折扣率", "实付金额"];
const MAX_NUMBER = 1e9;
const MEMBERS = ["VIP", "PLUS", "NORMAL"];

// 前端输入校验：拦截空值/非数字/负数/超范围/优惠券超额，返回错误信息或 null
function validateInput() {
  const rule = el("rule").value;
  if (!rule.trim()) return "规则不能为空";

  const amountText = el("amount").value.trim();
  const couponText = el("coupon").value.trim();
  if (amountText === "" || couponText === "") return "订单金额和优惠券不能为空";

  const amount = Number(amountText);
  const coupon = Number(couponText);
  if (!Number.isFinite(amount) || !Number.isFinite(coupon)) return "金额必须是有效数字";
  if (amount < 0 || coupon < 0) return "金额和优惠券不能为负数";
  if (amount > MAX_NUMBER || coupon > MAX_NUMBER) return "金额超出允许范围";
  if (coupon > amount) return "优惠券金额不得超过订单金额";
  if (!MEMBERS.includes(el("member").value)) return "会员等级非法";
  return null;
}

function showErrors(list) {
  errors.replaceChildren();
  for (const err of list) {
    const li = document.createElement("li");
    li.textContent = typeof err === "string" ? err : (err.message || JSON.stringify(err));
    errors.appendChild(li);
  }
}

function render(data) {
  result.hidden = false;
  const list = Array.isArray(data.errors) ? data.errors : [];
  const ok = data.status === "ok" && list.length === 0;
  badge.textContent = ok ? "结算成功" : "规则错误";
  badge.className = "badge " + (ok ? "ok" : "error");
  exit.textContent = data.exitCode === undefined || data.exitCode === null
    ? "" : `退出码 ${data.exitCode}`;

  // outputs 可能是非法数据，统一按数组处理；不足 4 项时安全兜底
  const outs = Array.isArray(data.outputs) ? data.outputs : [];
  const complete = ok && outs.length >= FIELDS.length
    && outs.every((v) => v !== null && v !== undefined && String(v).trim() !== "");
  paybox.hidden = !complete;
  payable.textContent = complete ? "¥" + outs[3] : "—";

  breakdown.replaceChildren();
  if (complete) {
    FIELDS.forEach((label, i) => {
      const row = document.createElement("div");
      row.className = "row";
      const k = document.createElement("span");
      k.className = "k";
      k.textContent = label;
      const v = document.createElement("span");
      v.className = "v";
      v.textContent = i === 2 ? outs[i] : "¥" + outs[i];
      row.appendChild(k);
      row.appendChild(v);
      breakdown.appendChild(row);
    });
  } else if (ok) {
    // status 为 ok 但输出不完整：视为数据异常，明确提示而不是显示半截结果
    const li = document.createElement("li");
    li.textContent = `规则输出不完整：期望 ${FIELDS.length} 项，实际 ${outs.length} 项`;
    errors.replaceChildren(li);
  }

  if (!ok || !complete) showErrors(list);
  else errors.replaceChildren();

  try {
    raw.textContent = JSON.stringify(data, null, 2);
  } catch (_) {
    raw.textContent = String(data);
  }
}

async function settle() {
  const bad = validateInput();
  if (bad) {
    result.hidden = false;
    badge.textContent = "输入错误";
    badge.className = "badge error";
    exit.textContent = "";
    paybox.hidden = true;
    payable.textContent = "—";
    breakdown.replaceChildren();
    showErrors([bad]);
    raw.textContent = "";
    conn.textContent = "请先修正输入";
    return;
  }

  runBtn.disabled = true;
  conn.textContent = "正在调用 Porism 执行定价规则…";
  try {
    const resp = await fetch("/api/eval", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        code: el("rule").value,
        params: {
          member: el("member").value,
          amount: el("amount").value.trim(),
          coupon: el("coupon").value.trim(),
        },
      }),
    });
    if (!resp.ok) {
      const text = await resp.text();
      throw new Error(`HTTP ${resp.status}: ${text}`);
    }
    render(await resp.json());
    conn.textContent = "已连接到本地后端 · 规则来自 rules.prsm";
  } catch (e) {
    result.hidden = false;
    badge.textContent = "连接失败";
    badge.className = "badge error";
    exit.textContent = "";
    paybox.hidden = true;
    payable.textContent = "—";
    breakdown.replaceChildren();
    showErrors([String(e)]);
    raw.textContent = "";
    conn.textContent = "后端不可达，请重新启动应用";
  } finally {
    runBtn.disabled = false;
  }
}

el("quit").addEventListener("click", async () => {
  conn.textContent = "正在退出…";
  try { await fetch("/api/shutdown", { method: "POST" }); } catch (_) { /* 服务可能已停 */ }
  document.body.innerHTML = '<p class="conn">已退出，可以关闭此窗口。</p>';
});

runBtn.addEventListener("click", settle);
for (const id of ["amount", "coupon"]) {
  el(id).addEventListener("keydown", (e) => {
    if (e.key === "Enter") settle();
  });
}

// 从后端加载规则配置文件，加载完成后自动结算一次
(async () => {
  try {
    const resp = await fetch("/api/rule");
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const data = await resp.json();
    el("rule").value = data.rule || "";
    conn.textContent = "后端就绪 · 规则已从 rules.prsm 载入";
  } catch (_) {
    conn.textContent = "规则加载失败，可直接编辑规则框";
  }
  settle();
})();

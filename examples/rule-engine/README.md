# Porism 规则引擎示例（前后端分离）

一个贴近实际开发的桌面示例：**业务规则写成 `.prsm` 配置文件，订单数据作为参数
注入，Porism 求值得到定价结果**。演示嵌入式求值引擎的核心价值——规则与宿主代码分离，
改规则无需改代码、无需重新编译。

## 场景

订单结算的定价规则（阶梯满减 + 会员折扣）写在 `rules.prsm` 里：

```
amount = max(0, float($amount))
coupon = min(max(0, float($coupon)), amount)   # 优惠券不超过订单金额
subtotal = amount - coupon
reduction = subtotal >= 200 ? 50 : (subtotal >= 100 ? 20 : 0)
rate = $member == "VIP" ? 0.8 : ($member == "PLUS" ? 0.9 : 1.0)
payable = round((subtotal - reduction) * rate, 2)
```

## 输入界限与错误处理

规则本身对数值做钳制（金额 ≥ 0、优惠券 ∈ [0, 金额]），即使绕过界面直接执行
也不会产生负数结果。在此之上还有两道校验：

- **前端**：提交前校验规则非空、金额为有限数字且非负、优惠券不超过金额、
  会员等级白名单；非法输入就地提示，不发起请求。结果不足 4 项或格式异常时
  安全兜底，不显示半截数据。
- **后端**：限制请求体与规则文件（各 8 KiB）、规则源码（4000 字符）、参数数量
  （最多 16 个）与值长度（最多 64 字符）、金额上限（十亿），校验类型、数值
  有限性与范围、枚举与交叉约束；非法输入返回 `input` 类结构化诊断，porism
  自身的语法/参数错误则保留原始类型、行号与列号。

界面展示三段内容，对应真实开发中的"配置 → 数据 → 结果"链路：
- **业务规则**：从 `rules.prsm` 加载，可直接在界面编辑
- **订单数据**：会员等级、订单金额、优惠券，注入为参数对象
- **结算结果**：实付金额与明细；规则有误时显示带类型、行号、列号的结构化诊断

改一条规则（如把 VIP 折扣 0.8 改成 0.7）并重新结算，实付金额立即变化，
宿主代码一行不动——这正是把求值内核嵌入业务系统的意义。

## 结构

```
rule-engine/
  main.py        启动器：拉起后端 + 打开应用窗口
  server.py      后端：本地 HTTP API（读取规则文件、调用 porism）
  rules.prsm     业务规则配置（可编辑）
  web/           前端：GUI 页面，只通过 fetch 调用后端
    index.html
    style.css
    app.js
  bin/porism.exe 求值内核
```

## 运行

```bash
python main.py
```

启动器随机选一个本地端口，用系统浏览器的“应用窗口”模式（无地址栏）打开界面。
若系统没有 Edge / Chrome / Chromium，退回默认浏览器。若 `bin/` 下没有内核，
回退使用 `core/dist/windows-x86_64/bin/porism.exe`，需先执行 `core/run.sh build`。

## API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/health` | 健康检查，返回后端与内核路径 |
| `GET` | `/api/rule` | 读取 `rules.prsm` 的规则源码 |
| `POST` | `/api/eval` | 请求体 `{"code": "…", "params": {"member":"VIP","amount":"260","coupon":"30"}}`，返回结果对象 |
| `POST` | `/api/shutdown` | 关闭后端服务 |

后端仅监听 `127.0.0.1`，供本机界面调用。

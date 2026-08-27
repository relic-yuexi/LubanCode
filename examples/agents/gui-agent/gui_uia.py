# -*- coding: utf-8 -*-
"""UIA(Windows UI Automation)控件树快照:ctypes 手调 IUIAutomation COM 壳。

接入路选型——ctypes 直调,不起 PowerShell 子进程跑 System.WindowsAutomation:

  1. 坐标口径。插件进程已是 per-monitor v2 DPI 感知(gui_backend 构造时开
     的),in-proc UIA 回的矩形与截图、SendInput 同一物理像素口径,点得中;
     PowerShell 子进程是另一只 DPI-unaware 进程,System.WindowsAutomation
     在高 DPI 屏上会把矩形虚拟化甚至给空矩形(微软文档承认的老账),拿它
     的 rect 去点必错位。
  2. 新旧。托管 System.Windows.Automation 微软自家都不建议新用;native
     IUIAutomation 是正路。
  3. 时延。in-proc COM 走几百节点的树是毫秒级;起 powershell.exe 加载 CLR
     每回一到三秒,Agent 循环每步都贵。
  4. 零依赖不变。ctypes 标准库;comtypes/pywinauto 才是要装的第三方。

代价是手排 COM vtable。下标照 Windows SDK 10.0.26100 的
um/UIAutomationClient.h 逐条对过(注释标了头文件行号),并拿真窗口对账:
  - IUIAutomation(行 18848):ElementFromHandle=6,get_ControlViewWalker=14
  - IUIAutomationTreeWalker(行 3449):GetFirstChildElement=4,
    GetNextSiblingElement=6
  - IUIAutomationElement(行 1591):get_CurrentControlType=21,
    get_CurrentName=23,get_CurrentClassName=30,
    get_CurrentBoundingRectangle=43,GetCurrentPattern=16,Release=2
  - IUIAutomationValuePattern(行 6806):get_CurrentValue=4
BoundingRectangle 的 tagRECT 在本 SDK 实测是四枚 LONG(与 GetWindowRect
分毫不差)——坊间"UIA 矩形是 double"说的是 provider 侧 UiaRect,别混。

折叠与帽子:纯容器(无名 pane/window/titlebar 一类)不占行,子孙照走;
深度帽防深树、元素帽防宽树、时间帽防卡树(无响应窗口的 UIA 查询会挂)。
超帽如实报"截断",教模型用 depth 收窄。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
import time

# ---------------------------------------------------------------------------
# 控件类型(UIAutomationClient.h 行 1318-1384)。短名用英文稳定拼写——
# LocalizedControlType 随系统语言变(中文系统回中文),对模型不稳定。
# ---------------------------------------------------------------------------
CONTROL_TYPE_NAMES = {
    50000: "button", 50001: "calendar", 50002: "checkbox", 50003: "combobox",
    50004: "edit", 50005: "hyperlink", 50006: "image", 50007: "listitem",
    50008: "list", 50009: "menu", 50010: "menubar", 50011: "menuitem",
    50012: "progressbar", 50013: "radiobutton", 50014: "scrollbar",
    50015: "slider", 50016: "spinner", 50017: "statusbar", 50018: "tab",
    50019: "tabitem", 50020: "text", 50021: "toolbar", 50022: "tooltip",
    50023: "tree", 50024: "treeitem", 50025: "custom", 50026: "group",
    50027: "thumb", 50028: "datagrid", 50029: "dataitem", 50030: "document",
    50031: "splitbutton", 50032: "window", 50033: "pane", 50034: "header",
    50035: "headeritem", 50036: "table", 50037: "titlebar", 50038: "separator",
}

# 发射规则三档(快照行收不收):
#   一档:交互元素无条件收——按钮/输入/勾选/下拉/页签/列表项/菜单项这些
#         模型要动的家伙,没名也收(类型+矩形本身就是定位信息)。
#   二档:其余一切带 Name 才收——文本/图片有名就是标签锚点;容器有名就是
#         标签化区块( tkinter 的输入框在 UIA 里就是一枚带名 TkChild pane),
#         都值得给模型看一眼。
#   三档:永不收——窗口本体(快照头已带标题)、标题栏(其子按钮照收)、
#         分隔条、滚动块,纯噪音。
ALWAYS_EMIT = {"button", "calendar", "checkbox", "combobox", "edit",
               "hyperlink", "listitem", "menu", "menuitem", "radiobutton",
               "slider", "spinner", "splitbutton", "tabitem", "treeitem",
               "dataitem"}
NEVER_EMIT = {"window", "titlebar", "separator", "thumb"}

# ValuePattern 只在值类控件上试(读输入框现值/下拉现选):两枚跨进程
# COM 调用一枚元素,别摊到全树。
VALUE_TYPES = {"edit", "combobox", "spinner"}

DEFAULT_DEPTH = 8      # tkinter 一类小窗 3 层够,大应用 8 层起步
MAX_DEPTH = 24         # 再深就该拆着看了
MAX_ELEMENTS = 400     # 收录行帽:400 行铺开已读不动,structured 留全量
MAX_VISITS = 1200      # 走访节点帽:全折叠的树也别无底洞地走
WALK_BUDGET_SECONDS = 8.0  # 时间帽:无响应窗口的 UIA 查询会挂,到点收兵


def should_emit(control_type: str, name: str) -> bool:
    if control_type in ALWAYS_EMIT:
        return True
    if control_type in NEVER_EMIT:
        return False
    return bool(name and name.strip())


def collect_tree(root_children, iter_children, read_props, max_depth,
                 release_node=None):
    """通用收集器:COM 后端与 FakeBackend 共用同一套折叠规则与帽子。

    root_children 是根(窗口)的孩子们,深度记 1;iter_children/read_props
    是后端各自的取子/读属性回调;release_node 可选(COM 路Release 引用)。
    回 (elements, truncated, reason, visited)。
    """
    elements: list[dict] = []
    state = {"visited": 0, "truncated": False, "reason": ""}
    deadline = time.monotonic() + WALK_BUDGET_SECONDS

    def visit(children, depth):
        for node in children:
            if state["truncated"]:
                return
            state["visited"] += 1
            if state["visited"] > MAX_VISITS:
                state["truncated"], state["reason"] = True, f"走访节点超 {MAX_VISITS} 帽"
                return
            if time.monotonic() > deadline:
                state["truncated"], state["reason"] = True, f"走树超 {WALK_BUDGET_SECONDS:.0f}s 预算(目标窗口可能无响应)"
                return
            props = read_props(node)
            if props is not None and should_emit(props["control_type"], props.get("name", "")):
                if len(elements) >= MAX_ELEMENTS:
                    state["truncated"], state["reason"] = True, f"收录元素超 {MAX_ELEMENTS} 帽"
                    return
                elements.append({**props, "depth": depth})
            if depth < max_depth:
                visit(iter_children(node), depth + 1)
            if release_node is not None:
                release_node(node)

    visit(root_children, 1)
    return elements, state["truncated"], state["reason"], state["visited"]


# ---------------------------------------------------------------------------
# COM 壳(仅 Windows 真后端走到这;import 本模块在任何平台都不碰 windll)
# ---------------------------------------------------------------------------
class _UiaRect(ctypes.Structure):
    """IUIAutomationElement::CurrentBoundingRectangle 的出参。四枚 LONG,
    实测与 GetWindowRect 对账分毫不差(见模块头)。"""
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class _Guid(ctypes.Structure):
    _fields_ = [("data1", ctypes.c_ulong), ("data2", ctypes.c_ushort),
                ("data3", ctypes.c_ushort), ("data4", ctypes.c_ubyte * 8)]


def _guid(spec: str) -> _Guid:
    import ctypes as _ct
    guid = _Guid()
    _ct.OleDLL("ole32").CLSIDFromString(spec, ctypes.byref(guid))
    return guid


class _UiaSession:
    """一次快照一场 COM:CoInitialize → CoCreateInstance → 拿 ControlViewWalker。

    进程件件短命(插件每调用一只进程),不配 CoUninitialize 收尾——进程
    退出即回收,省的是每个元素的 Release 都得做对的心智负担。
    """

    CLSID_CUIAUTOMATION = "{ff48dba4-60ef-4201-aa87-54103eef594e}"
    IID_IUIAUTOMATION = "{30cbe57d-d9d0-452a-ab13-7ac5ac4825ee}"
    UIA_VALUE_PATTERN_ID = 10002

    def __init__(self) -> None:
        if sys.platform != "win32":
            raise OSError("uia_requires_windows")
        # ole32 走 OleDLL(那几个 Co* 都真回 HRESULT,失败自动抛,正合用)。
        # oleaut32 必须走 WinDLL:SysFreeString 返回 void,RAX 是残渣,
        # OleDLL 会把残渣当 HRESULT 验——带符号位就抛 OSError,五五开,
        # 挂相还是堆地址模样的乱码。这坑踩了一下午,立此存照。
        self.ole32 = ctypes.OleDLL("ole32")
        self.oleaut32 = ctypes.WinDLL("oleaut32")
        try:
            # COINIT_APARTMENTTHREADED:UIA 客户端的文档正路;插件进程一条
            # 主线程跑完就走,不碰消息循环的边。
            self.ole32.CoInitializeEx(None, 0x2)
        except OSError:
            pass  # RPC_E_CHANGED_MODE:线程已被别的模式初始化,UIA 照用
        self.automation = ctypes.c_void_p()
        self.ole32.CoCreateInstance(
            ctypes.byref(_guid(self.CLSID_CUIAUTOMATION)), None, 0x1,  # INPROC_SERVER
            ctypes.byref(_guid(self.IID_IUIAUTOMATION)), ctypes.byref(self.automation))
        if not self.automation.value:
            raise OSError("uia_init_failed: CoCreateInstance 拿到空 IUIAutomation")
        self.read_failures: list[str] = []
        self.walker = ctypes.c_void_p()
        self._call(self.automation, 14, "get_ControlViewWalker", ctypes.HRESULT,
                   ctypes.POINTER(ctypes.c_void_p))(self.automation, ctypes.byref(self.walker))
        if not self.walker.value:
            raise OSError("uia_init_failed: ControlViewWalker 为空")

    # -- vtable 小工 --------------------------------------------------------
    def _call(self, obj, index, what, restype, *argtypes):
        """取 vtable[index] 函数指针。restype 传 HRESULT 时 ctypes 对失败码
        自动抛 OSError;_hr 只在成功路径补一层上下文(双保险)。"""
        interface = ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))
        table = ctypes.cast(interface[0], ctypes.POINTER(ctypes.c_void_p))
        return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(table[index])

    def _hr(self, hr: int, what: str) -> None:
        if hr != 0:
            raise OSError(f"uia_error({what}): hr=0x{hr & 0xFFFFFFFF:08X}")

    def _bstr(self, element, index, what) -> str:
        raw = ctypes.c_void_p()
        self._hr(self._call(element, index, what, ctypes.HRESULT,
                            ctypes.POINTER(ctypes.c_void_p))(element, ctypes.byref(raw)), what)
        if not raw.value:
            return ""
        try:
            return ctypes.wstring_at(raw.value)  # BSTR 保准 NUL 结尾
        finally:
            self.oleaut32.SysFreeString(raw)  # 别人的 BSTR,读完还账

    def _release(self, element) -> None:
        if element is not None and element.value:
            self._call(element, 2, "Release", ctypes.c_ulong)(element)

    # -- 树走查 --------------------------------------------------------------
    def element_from_handle(self, hwnd: int):
        element = ctypes.c_void_p()
        self._hr(self._call(self.automation, 6, "ElementFromHandle", ctypes.HRESULT,
                            wt.HWND, ctypes.POINTER(ctypes.c_void_p))(
            self.automation, wt.HWND(hwnd), ctypes.byref(element)), "ElementFromHandle")
        return element

    def children_of(self, element):
        """首孩+逐弟,收成一列表(走访时逐枚 Release)。活树会抖:窗口关了、
        页签翻了、Tk 重建 helper HWND——哪一枪喂回失败 HRESULT,就当这条
        链到此为止,收下已到手的,不炸整份快照。"""
        children = []
        child = ctypes.c_void_p()
        try:
            self._call(self.walker, 4, "GetFirstChildElement", ctypes.HRESULT,
                       ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(
                self.walker, element, ctypes.byref(child))
            guard = 0
            while child.value and guard <= MAX_VISITS:
                children.append(child)
                nxt = ctypes.c_void_p()
                self._call(self.walker, 6, "GetNextSiblingElement", ctypes.HRESULT,
                           ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(
                    self.walker, child, ctypes.byref(nxt))
                child = nxt
                guard += 1
        except OSError:
            pass  # 断链就断链,树还在抖说明本来也不可靠
        return children

    def read_props(self, element):
        """读一枚元素的类型/名/类名/矩形;值类控件再试 ValuePattern 现值。
        读挂了回 None(调用方跳过该元素)——活树上单枚控件失联是常事
        (窗口关了、页签翻了)。挂相记进 read_failures(截前 8 笔),快照
        空树时给排错留线索。"""
        try:
            return self._read_props(element)
        except OSError as error:
            code = error.winerror if error.winerror is not None else error.errno
            if len(self.read_failures) < 8:
                self.read_failures.append(f"{hex(code & 0xFFFFFFFF) if code is not None else repr(error)}")
            return None

    def _read_props(self, element):
        type_id = ctypes.c_int()
        self._hr(self._call(element, 21, "get_CurrentControlType", ctypes.HRESULT,
                            ctypes.POINTER(ctypes.c_int))(element, ctypes.byref(type_id)),
                 "CurrentControlType")
        name = self._bstr(element, 23, "CurrentName")
        class_name = self._bstr(element, 30, "CurrentClassName")
        rect = _UiaRect()
        self._hr(self._call(element, 43, "get_CurrentBoundingRectangle", ctypes.HRESULT,
                            ctypes.POINTER(_UiaRect))(element, ctypes.byref(rect)),
                 "CurrentBoundingRectangle")
        control_type = CONTROL_TYPE_NAMES.get(type_id.value, f"type{type_id.value}")
        value = None
        if control_type in VALUE_TYPES:
            pattern = ctypes.c_void_p()
            hr = self._call(element, 16, "GetCurrentPattern", ctypes.HRESULT,
                            ctypes.c_int, ctypes.POINTER(ctypes.c_void_p))(
                element, self.UIA_VALUE_PATTERN_ID, ctypes.byref(pattern))
            if hr == 0 and pattern.value:
                try:
                    value = self._bstr(pattern, 4, "ValuePattern.CurrentValue")
                finally:
                    self._release(pattern)
        return {"control_type": control_type, "name": name, "class_name": class_name,
                "rect": [rect.left, rect.top, rect.right, rect.bottom],
                "value": value}


def snapshot_window(window_id: str, max_depth: int) -> dict:
    """真桌面路:窗口 id(0x 十六进制)→ 收好的元素表。全折衷量(空树、
    截断、耗时)如实回,不冒充。"""
    started = time.monotonic()
    session = _UiaSession()
    root = session.element_from_handle(int(window_id, 16))
    try:
        root_children = session.children_of(root)
        elements, truncated, reason, visited = collect_tree(
            root_children,
            lambda node: session.children_of(node),
            session.read_props,
            max_depth,
            release_node=session._release)
    finally:
        session._release(root)
    return {"elements": elements, "truncated": truncated, "reason": reason,
            "visited": visited, "elapsed_ms": int((time.monotonic() - started) * 1000),
            "read_failures": session.read_failures}

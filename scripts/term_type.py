#!/usr/bin/env python3
"""
opencli 终端注入工具 —— 将字符串逐字符注入到 Canvas VNC 终端。

用法:
  python scripts/term_type.py "gcc -std=c11 -fopenmp -o test test.c -lm"
  python scripts/term_type.py --session work "make release"
  python scripts/term_type.py --enter-only              # 仅按回车
  python scripts/term_type.py --ctrl-c                  # Ctrl+C
  python scripts/term_type.py "ls -la /tmp" --screenshot test.png
"""

import argparse
import os
import subprocess
import sys
import time

# 确保 stdout 用 utf-8
if sys.stdout.encoding != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# ── 定位 opencli ──────────────────────────
_OPENCLI_BIN = os.environ.get('OPENCLI_BIN', 'opencli')


# ── 字符 → opencli keys 参数映射 ──────────
KEY_MAP = {
    ' ':  'Space',
    '\n': 'Enter',
    '\t': 'Tab',
    '\b': 'Backspace',
    '\x1b': 'Escape',    # ESC
    '\x03': None,        # Ctrl+C 单独处理
    # 特殊键名字
    'Enter':   'Enter',
    'Escape':  'Escape',
    'Tab':     'Tab',
    'Space':   'Space',
    'Backspace':'Backspace',
    'Delete':  'Delete',
    'Home':    'Home',
    'End':     'End',
    'ArrowUp':   'ArrowUp',
    'ArrowDown': 'ArrowDown',
    'ArrowLeft': 'ArrowLeft',
    'ArrowRight':'ArrowRight',
    'PageUp':   'PageUp',
    'PageDown': 'PageDown',
}


def _opencli(session: str, args: list[str], timeout: int = 10) -> subprocess.CompletedProcess:
    """运行 opencli 命令，返回 CompletedProcess。"""
    cmd = [_OPENCLI_BIN, 'browser', session] + args
    return subprocess.run(
        cmd, capture_output=True, text=True, timeout=timeout, shell=True
    )


def press_key(session: str, key_name: str, delay: float = 0.02) -> bool:
    """按下单个按键。返回 True 表示成功。"""
    result = _opencli(session, ['keys', key_name])
    ok = 'Pressed:' in (result.stdout or '')
    if delay > 0:
        time.sleep(delay)
    return ok


def press_combo(session: str, combo: str, delay: float = 0.02) -> bool:
    """按下组合键，如 Control+c, Shift+a。"""
    result = _opencli(session, ['keys', combo])
    ok = 'Pressed:' in (result.stdout or '')
    if delay > 0:
        time.sleep(delay)
    return ok


def focus_canvas(session: str) -> bool:
    """聚焦 Canvas，让终端接收按键。"""
    js = "(function(){var c=document.querySelector('#screen canvas');if(!c)return'no canvas';c.focus();c.click();return'focused'})()"
    result = _opencli(session, ['eval', js])
    return 'focused' in (result.stdout or '')


def type_text(session: str, text: str, char_delay: float = 0.02) -> int:
    """
    逐字符将文本注入终端。
    返回成功注入的字符数。
    """
    count = 0
    i = 0
    while i < len(text):
        ch = text[i]

        # 处理组合键序列 <Ctrl+c>, <Enter> 等
        if ch == '<' and i + 1 < len(text):
            end = text.find('>', i)
            if end > i:
                combo = text[i+1:end]
                press_combo(session, combo, char_delay)
                i = end + 1
                count += end - i + 1
                continue

        # 单字符映射
        key_name = KEY_MAP.get(ch, ch)
        if key_name is None:
            i += 1
            continue

        if not press_key(session, key_name, char_delay):
            print(f"  ⚠ 按键失败: {repr(ch)} → {key_name}", file=sys.stderr)

        i += 1
        count += 1

    return count


def take_screenshot(session: str, path: str) -> bool:
    """截图保存到指定路径。"""
    result = _opencli(session, ['screenshot', path], timeout=30)
    return result.returncode == 0


# ── 主入口 ───────────────────────────────
def main() -> int:
    parser = argparse.ArgumentParser(
        description='opencli 终端注入工具 —— 逐字符输入到 Canvas VNC 终端',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s "gcc -std=c11 -fopenmp -o test test.c -lm"
  %(prog)s "ls -la" --enter
  %(prog)s --ctrl-c              # 取消当前命令
  %(prog)s "make release" --screenshot build_result.png
  %(prog)s --enter-only          # 仅回车
  %(prog)s "<Control+c>"         # 用尖括号表示组合键
        """,
    )
    parser.add_argument('text', nargs='?', default='',
                        help='要注入的文本。支持 <Control+c> <Shift+a> 等组合键标记')
    parser.add_argument('--session', '-s', default='work',
                        help='opencli 会话名 (默认: work)')
    parser.add_argument('--delay', '-d', type=float, default=0.02,
                        help='字符间隔秒数 (默认: 0.02)')
    parser.add_argument('--enter', '-e', action='store_true',
                        help='文本输入后自动按回车')
    parser.add_argument('--enter-only', action='store_true',
                        help='仅按回车')
    parser.add_argument('--ctrl-c', action='store_true',
                        help='发送 Ctrl+C')
    parser.add_argument('--screenshot', default='',
                        help='截图保存路径')
    parser.add_argument('--no-focus', action='store_true',
                        help='跳过 Canvas 聚焦步骤')

    args = parser.parse_args()

    # 仅 Ctrl+C
    if args.ctrl_c:
        print('Ctrl+C')
        press_combo(args.session, 'Control+c', 0)
        return 0

    # 仅回车
    if args.enter_only:
        print('Enter')
        press_key(args.session, 'Enter', 0)
        return 0

    if not args.text:
        parser.print_help()
        return 0

    # 聚焦 Canvas
    if not args.no_focus:
        if not focus_canvas(args.session):
            print('⚠ Canvas 聚焦失败，继续尝试...', file=sys.stderr)

    # 注入文本
    print(f'注入: {args.text[:80]}{"..." if len(args.text) > 80 else ""}')
    count = type_text(args.session, args.text, args.delay)
    print(f'  ✓ {count} 字符已发送')

    # 回车
    if args.enter:
        press_key(args.session, 'Enter', 0)
        print('  ✓ Enter')

    # 截图
    if args.screenshot:
        time.sleep(0.5)
        if take_screenshot(args.session, args.screenshot):
            print(f'  ✓ 截图: {args.screenshot}')
        else:
            print(f'  ✗ 截图失败', file=sys.stderr)

    return 0


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""把明文對照表（日文<TAB>中文）轉成雜湊版（雜湊<TAB>中文）。

散布版的對照表不含任何遊戲原文，只留「原文的雜湊」與譯文。
雜湊是 FNV-1a 64 位元，以字串長度起始，與 a9tc_dist.c 的 hash_str() 完全一致。

用法：./make-table.py 明文表.txt > a9tc.txt
"""
import sys

def hash_str(s: str) -> int:
    h = (1469598103934665603 ^ len(s)) & 0xFFFFFFFFFFFFFFFF
    for ch in s:
        h = ((h ^ ord(ch)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

HEADER = """# A列車で行こう9 V5 繁體中文化 — 對照表（雜湊版）
# 第一欄是日文原文的雜湊值，不含原文本身。
# 以 # 開頭為設定或註解。
#
#charset=1
#delay=20        # 啟動後幾秒開始改寫；電腦較慢可以調大
#patch           # 拿掉這行就只會替換字型、不做翻譯
#font=           # 留空 = 自動挑系統裝得到的繁中字型
#fontscale=100   # 字型大小百分比，100 為原樣；調太大會被固定行高裁切
#
"""

def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    out, seen, dup = [], set(), 0
    for line in open(sys.argv[1], encoding="utf-8"):
        if line.startswith("#") or "\t" not in line: continue
        src, dst = line.rstrip("\n").split("\t", 1)
        h = hash_str(src)
        if h in seen: dup += 1; continue      # 雜湊相同代表原文相同，或極罕見的碰撞
        seen.add(h)
        out.append(f"{h:016x}\t{dst}")
    sys.stdout.write(HEADER + "\n".join(out) + "\n")
    print(f"{len(out)} 條，跳過重複 {dup} 條", file=sys.stderr)

main()

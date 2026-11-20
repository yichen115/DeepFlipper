"""歌名模糊匹配（worker 与测试共用；服务端插件里有一份同样规则的副本）。"""
from difflib import SequenceMatcher
import re
import unicodedata

_DROP = re.compile(r"[\s\-_·•.,，。!！?？'\"“”‘’《》<>()（）\[\]【】~～&+]+")
_TAGS = re.compile(r"[(\[（【](live|伴奏|纯音乐|cover|翻唱|现场|remix|inst\w*|official.*?|高品质|无损|hq|sq|mqms\d*|\d{1,2})[)\]）】]", re.I)


def normalize(text):
    text = unicodedata.normalize("NFKC", str(text or "")).lower()
    text = _TAGS.sub("", text)
    return _DROP.sub("", text)


def score(query, title):
    """0–1：完全相同 1；包含关系 0.7+；否则按编辑相似度打折。"""
    best = 0.0
    q = normalize(query)
    if not q:
        return 0.0
    # “周杰伦 - 晴天” 这类文件名：整体和每一段分别比较
    parts = [title] + [p for p in re.split(r"\s+-\s+|_-_|–|—", str(title)) if p.strip()]
    for part in parts:
        t = normalize(part)
        if not t:
            continue
        if q == t:
            return 1.0
        if q in t:
            s = 0.7 + 0.3 * len(q) / len(t)
        elif t in q:
            s = 0.65 + 0.3 * len(t) / len(q)
        else:
            s = 0.8 * SequenceMatcher(None, q, t).ratio()
        best = max(best, s)
    return best


def best_match(query, titles, threshold=0.5):
    """返回 (title, score)；没有足够像的返回 None。"""
    ranked = sorted(((score(query, t), t) for t in titles), key=lambda x: -x[0])
    if ranked and ranked[0][0] >= threshold:
        return ranked[0][1], ranked[0][0]
    return None


def suggestions(query, titles, limit=5):
    ranked = sorted(titles, key=lambda t: -score(query, t))
    return ranked[:limit]

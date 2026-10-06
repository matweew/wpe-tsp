/*
 * Shared look and d-pad navigation for the app's internal pages (wpe-tsp://history,
 * wpe-tsp://downloads), so they match the on-screen keyboard and menus.
 */
#pragma once

/* <head> up to and including <body>; printf argument: the page title. */
#define PAGE_HEAD \
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>" \
    "<title>%s</title><style>" \
    "body{background:#202124;color:#e8eaed;font-family:sans-serif;margin:0;padding:16px 24px 56px}" \
    "h1{font-size:1.3em;color:#8ab4f8;margin:0 0 12px}" \
    "a,.item{display:block;text-decoration:none;color:inherit;padding:10px 14px;border-radius:8px;outline:none}" \
    "a:hover{background:#3c4043}" /* under the pointer: subtle */ \
    "a:focus{background:#8ab4f8;color:#202124}" /* d-pad selection (what START opens) */ \
    ".t{font-size:1.05em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}" \
    ".s{font-size:.85em;color:#9aa0a6;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}" \
    "a:focus .s{color:#3c4043}" \
    ".bar{height:4px;background:#3c4043;border-radius:2px;margin-top:6px}" \
    ".bar div{height:4px;background:#8ab4f8;border-radius:2px}" \
    ".warn{color:#f28b82}.clear{margin-top:16px;color:#f28b82}.empty{color:#9aa0a6;padding:10px 14px}" \
    ".hint{position:fixed;left:0;right:0;bottom:0;padding:10px 24px;background:#292a2d;color:#9aa0a6;" \
    "font-size:.85em;border-top:1px solid #3c4043}.hint b{color:#e8eaed}" \
    "</style></head><body>"

/* The d-pad sends arrow keys: move focus through the page's links; START (Enter) opens the
 * focused one. The first link starts focused. B goes back (handled by the browser for
 * wpe-tsp:// pages). */
#define PAGE_TAIL \
    "<div class='hint'><b>D-pad</b> select &nbsp;&middot;&nbsp; <b>START</b> open &nbsp;&middot;&nbsp; <b>B</b> back</div>" \
    "<script>" \
    "const links=[...document.querySelectorAll('a')];" \
    "if(links.length)links[0].focus();" \
    "document.addEventListener('keydown',e=>{" \
    "const d=e.key==='ArrowDown'?1:e.key==='ArrowUp'?-1:0;if(!d||!links.length)return;" \
    "const i=links.indexOf(document.activeElement);" \
    "const n=links[Math.max(0,Math.min(links.length-1,(i<0?0:i+d)))];" \
    "n.focus();n.scrollIntoView({block:'nearest'});e.preventDefault();});" \
    "</script></body></html>"

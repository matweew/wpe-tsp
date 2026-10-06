#!/usr/bin/env python3
"""Convert EasyList-format lists into a WebKit content-blocker rule list (JSON).

Only domain rules are converted: they block the ad/tracker requests themselves, which is where
the memory and CPU go on this device. Element-hiding (##) and rules with other options are skipped.
  ||ads.example.com^                 -> block (any request to that host or its subdomains)
  ||ads.example.com^$third-party     -> block when loaded by another site
  @@||example.com^...                -> exception (ignore-previous-rules), placed after all blocks

Usage: make-adblock.py <out.json> <list.txt>... [--max N]
"""
import json
import re
import sys

DOMAIN_RULE = re.compile(r"^\|\|([a-z0-9][a-z0-9.-]*[a-z0-9])\^(\$third-party)?$")
EXCEPTION_RULE = re.compile(r"^@@\|\|([a-z0-9][a-z0-9.-]*[a-z0-9])\^")


def url_filter(domain):
    # scheme://[subdomains.]domain followed by a port, path or end
    return "^[^:]+://+([^:/]+\\.)?" + re.escape(domain).replace("\\-", "-") + "[:/]"


def main():
    args = sys.argv[1:]
    limit = None
    if "--max" in args:
        i = args.index("--max")
        limit = int(args[i + 1])
        del args[i:i + 2]
    out, lists = args[0], args[1:]

    blocks, third_party, exceptions = {}, set(), set()
    for path in lists:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip().lower()
                m = DOMAIN_RULE.match(line)
                if m:
                    domain = m.group(1)
                    if m.group(2):
                        third_party.add(domain)
                    blocks[domain] = None  # keeps first-seen order
                    continue
                m = EXCEPTION_RULE.match(line)
                if m:
                    exceptions.add(m.group(1))

    domains = list(blocks)
    if limit:
        domains = domains[:limit]
    # A domain blocked unconditionally anywhere doesn't need the third-party restriction
    unconditional = {d for d in domains if d not in third_party}

    rules = []
    for domain in domains:
        trigger = {"url-filter": url_filter(domain)}
        if domain not in unconditional:
            trigger["load-type"] = ["third-party"]
        rules.append({"trigger": trigger, "action": {"type": "block"}})
    for domain in sorted(exceptions):
        rules.append({"trigger": {"url-filter": url_filter(domain)}, "action": {"type": "ignore-previous-rules"}})

    with open(out, "w") as f:
        json.dump(rules, f, separators=(",", ":"))
    print(f"{out}: {len(rules)} rules ({len(domains)} blocked domains, {len(exceptions)} exceptions)")


if __name__ == "__main__":
    main()

import urllib.request
import re

url = "https://components.espressif.com/components/espressif/cjson/versions/1.7.19~2"
text = urllib.request.urlopen(url, timeout=20).read().decode("utf-8", "replace")
print("len", len(text))
for match in re.findall(r'https?://[^"<> ]+', text)[:30]:
    print(match)
print("--- cjson refs ---")
for match in list(re.finditer("cjson", text, re.I))[:12]:
    print(text[max(0, match.start() - 100): match.start() + 180].replace("\n", " "))

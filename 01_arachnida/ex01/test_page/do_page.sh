#!/usr/bin/env bash
cat > index.html <<'HTML'
<!doctype html><html><body>
  <img src="img/img_01.jpg">
  <a href="page2.html">next</a>
</body></html>
HTML

cat > page2.html <<'HTML'
<!doctype html><html><body>
  <img src="img/img_02.PNG">
  <a href="page3.html">deeper</a>
</body></html>
HTML

cat > page3.html <<'HTML'
<!doctype html><html><body><img src="img/img_03.jpg"></body></html>
HTML

python3 -m http.server 8000
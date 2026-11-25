#!/usr/bin/env bash
cat > index.html <<'HTML'
<!doctype html><html><body>
  <img src="img/img_01.jpg">
  <a href="page2.html">next</a>
  <a href="https://www.42barcelona.com/">42</a>
</body></html>
HTML

cat > page2.html <<'HTML'
<!doctype html><html><body>
  <img src="img/img_02.PNG">
  <a href="page3.html">deeper</a>
  <a href="https://upload.wikimedia.org/wikipedia/commons/7/71/Sibirischer_tiger_de_edit02.jpg">out</a>
</body></html>
HTML

cat > page3.html <<'HTML'
<!doctype html><html><body><img src="img/img_03.jpg"></body></html>
HTML

python3 -m http.server 8000
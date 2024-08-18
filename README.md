# LiveGate

Ein minimaler HTTP-Server, der das Entwickeln von Webseiten mit allen möglichen Editoren vereinfacht.
Er erkennt Änderungen an Dateien und lädt im Browser geöffnete Seiten daraufhin neu.

Es handelt sich um ein interenes Entwicklungs-Tool der YesGate UG, das nicht für die öffentliche Verwendung bestimmt ist.
Bugs dürfen demnach vorhanden sein, solange sie sich nicht negativ auf die Entwicklung auswirken.

## Merkmale
* Scroll-Offset wird beim Neu-Laden wiederhergestellt, damit die Ansicht gleich bleibt
* SASS-Kompilierung wird unterstützt (der SASS-Compiler kann auch in Docker ausgeführt werden)
* Grundlegende TypeScript-Kompilierung wird unterstützt


## Installation

```bash
git clone git@github.com:jhlgns/livegate.git
cd livegate
git submodule update --init
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="~/.local" ..
make install
```

`.local` zum `PATH` hinzufügen:
```bash
echo export PATH="$PATH:$HOME/.local/bin" >> ~/.bashrc
source ~/.bashrc
```

## Verwendung
```bash
# Hilfe anzeigen
livegate -h

# ~/repos/my-website/public-html auf dem Standard-Port hosten
livegate --content-dir ~/repos/my-website/public-html
```

## Dateien
* sass-map.txt
  * Beinhaltet die SASS-Verzeichniszuweisungen. Der Inhalt wird in den ```sass --watch ...```  Befehl eingefügt.
    Für eine genaue Dokumentation, was in dieser Datei stehen sollte, bitte die SASS-Dokumentation heranziehen.

#!/bin/sh
# Adds changelog.txz and bogons.txz, signed with our key, to every aarch64
# sets folder of an update server tree. OPNsense only publishes them for
# amd64; both are CPU-independent, so we take those and add our own release
# notes from update-server/changelog/<release>.txt to the changelog.
# Usage: publish-changelog.sh <www-root>

set -e

WWW=${1:?usage: publish-changelog.sh <www-root>}
KEYDIR=${KEYDIR:-/root/signing}
NOTES=${NOTES:-$(dirname $(realpath ${0}))/update-server/changelog}
UPSTREAM=${UPSTREAM:-https://pkg.opnsense.org}
SIGNCMD="/usr/tools/scripts/pkg_sign.sh ${KEYDIR}/repo.pub ${KEYDIR}/repo.key"

[ -f "${KEYDIR}/repo.key" ] || { echo "ERROR: no signing key in ${KEYDIR}"; exit 1; }

sign_file()
{
	sha256 -q "${1}" | ${SIGNCMD} > "${1}.sig"
}

TMP=$(mktemp -d /tmp/changelog.XXXXXX)
trap 'rm -rf ${TMP}' EXIT INT TERM

for DIR in ${WWW}/FreeBSD:*:aarch64/*/; do
	[ -d "${DIR}latest" ] || continue
	SETS=${DIR}sets
	mkdir -p ${SETS}
	SERIES=$(basename $(dirname ${SETS}))
	ABI=$(basename $(dirname $(dirname ${SETS})))
	SRC=${UPSTREAM}/${ABI%:aarch64}:amd64/${SERIES}/sets
	rm -rf ${TMP}/*

	echo ">>> ${ABI}/${SERIES}: bogons"
	fetch -q -o ${TMP}/bogons.txz ${SRC}/bogons.txz
	tar -tJf ${TMP}/bogons.txz > /dev/null
	cp ${TMP}/bogons.txz ${SETS}/bogons.txz
	sign_file ${SETS}/bogons.txz

	echo ">>> ${ABI}/${SERIES}: changelog + $(ls ${NOTES} | wc -l | tr -d ' ') release notes"
	fetch -q -o ${TMP}/changelog.txz ${SRC}/changelog.txz
	mkdir ${TMP}/cl
	tar -C ${TMP}/cl -xJf ${TMP}/changelog.txz
	python3 - ${TMP}/cl ${NOTES} <<'EOF'
import datetime, html, json, os, sys

cl, notes = sys.argv[1], sys.argv[2]
index = json.load(open(os.path.join(cl, "index.json")))
for name in sorted(os.listdir(notes)):
    if not name.endswith(".txt"):
        continue
    version = name[:-4]
    lines = open(os.path.join(notes, name)).read().splitlines()
    day = datetime.date.today()
    if lines and lines[0].startswith("date:"):
        day = datetime.date.fromisoformat(lines.pop(0)[5:].strip())
    text = "\n".join(lines).strip() + "\n"

    # Blank lines split paragraphs, "- " lines become a list.
    out, items = [], []
    for block in text.split("\n\n"):
        rows = block.strip().splitlines()
        if rows and all(r.startswith("- ") for r in rows):
            out.append("<ul>" + "".join("<li>" + html.escape(r[2:]) + "</li>" for r in rows) + "</ul>")
        elif rows:
            out.append("<p>" + html.escape(" ".join(rows)) + "</p>")
    open(os.path.join(cl, version + ".htm"), "w").write("".join(out))
    open(os.path.join(cl, version + ".txt"), "w").write(text)

    index = [e for e in index if e["version"] != version]
    index.append({"series": ".".join(version.split(".")[:2]), "version": version,
                  "date": day.strftime("%B %d, %Y")})
json.dump(index, open(os.path.join(cl, "index.json"), "w"), indent=0)
EOF
	tar -C ${TMP}/cl -cJf ${SETS}/changelog.txz .
	sign_file ${SETS}/changelog.txz
done

echo ">>> done"

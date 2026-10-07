#!/bin/sh
# Recompila o DSDT (precisa do iasl, pacote acpica-tools) e gera
# core/src/acpi/dsdt_aml.h. Rode apos alterar core/src/acpi/dsdt.asl.
set -e
cd "$(dirname "$0")/../src/acpi"
tmp=$(mktemp -d)
iasl -p "$tmp/dsdt" dsdt.asl >/dev/null
{
    echo "/* Gerado por core/tools/gen_dsdt.sh a partir de dsdt.asl. Nao edite. */"
    echo "static const unsigned char dsdt_aml[] = {"
    od -An -v -tx1 "$tmp/dsdt.aml" | sed 's/ *\([0-9a-f][0-9a-f]\)/0x\1, /g; s/^/    /; s/ *$//'
    echo "};"
} > dsdt_aml.h
rm -rf "$tmp"
echo "dsdt_aml.h: $(wc -c < dsdt.asl) bytes de ASL -> $(grep -o 0x dsdt_aml.h | wc -l) bytes de AML"

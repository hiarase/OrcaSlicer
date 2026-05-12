#!/bin/sh

#  Snapmaker_Orca gettext
#  Created by SoftFever on 27/5/23.
#

find_gettext_tool()
{
    tool="$1"
    if command -v "$tool" >/dev/null 2>&1; then
        command -v "$tool"
        return 0
    fi

    for prefix in /opt/homebrew/opt/gettext /usr/local/opt/gettext; do
        if [ -x "$prefix/bin/$tool" ]; then
            echo "$prefix/bin/$tool"
            return 0
        fi
    done

    return 1
}

MSGFMT=${MSGFMT:-$(find_gettext_tool msgfmt)}
MSGMERGE=${MSGMERGE:-$(find_gettext_tool msgmerge)}
XGETTEXT=${XGETTEXT:-$(find_gettext_tool xgettext)}

if [ -z "$MSGFMT" ]; then
    echo "msgfmt was not found. Install gettext or set MSGFMT to the msgfmt path."
    exit 1
fi

# Check for --full argument
FULL_MODE=false
for arg in "$@"
do
    if [ "$arg" = "--full" ]; then
        FULL_MODE=true
    fi
done

if $FULL_MODE; then
    if [ -z "$XGETTEXT" ] || [ -z "$MSGMERGE" ]; then
        echo "xgettext/msgmerge were not found. Install gettext or set XGETTEXT/MSGMERGE."
        exit 1
    fi
    "$XGETTEXT" --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f ./localization/i18n/list.txt -o ./localization/i18n/Snapmaker_Orca.pot
    python3 scripts/HintsToPot.py ./resources ./localization/i18n
fi


echo "$0: working dir = $PWD"
pot_file="./localization/i18n/Snapmaker_Orca.pot"
for dir in ./localization/i18n/*/
do
    dir=${dir%*/}      # remove the trailing "/"
    lang=${dir##*/}    # extract the language identifier

    if [ -f "$dir/Snapmaker_Orca_${lang}.po" ]; then
        if $FULL_MODE; then
            "$MSGMERGE" -N -o "$dir/Snapmaker_Orca_${lang}.po" "$dir/Snapmaker_Orca_${lang}.po" "$pot_file"
        fi
        mkdir -p "resources/i18n/${lang}"
        if ! "$MSGFMT" --check-format -o "resources/i18n/${lang}/Snapmaker_Orca.mo" "$dir/Snapmaker_Orca_${lang}.po"; then
            echo "Error encountered with msgfmt command for language ${lang}."
            exit 1  # Exit the script with an error status
        fi
    fi
done

#!/bin/bash

set -e

read -p "Entrez le nom de la police : " FAMILY_NAME

while true; do
    read -p "Voulez-vous compiler en mode 2 bit (anticrénelage)? [o/n] " on
    case $on in
        [Oo]* ) two_bit=true;  break;;
        [Nn]* ) two_bit=false; break;;
        * ) echo "Il faut répondre avec un O ou N.";;
    esac
done

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
FONT_SIZES=(12 14 16 18)

for size in ${FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do

    file=$(find . -type f -name "${FAMILY_NAME}-${style}.*")

    if [[ $file =~ \.ttf$ ]]; then
      font_path="${FAMILY_NAME}-${style}.ttf"
    elif [[ $file =~ \.otf$ ]]; then
      font_path="${FAMILY_NAME}-${style}.otf"
    else
      echo "Aucun fichier .otf ni .ttf pour ${FAMILY_NAME}-${style} trouvé, le programme va le sauter"
    fi

    font_name="${FAMILY_NAME}_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    output_path="${font_name}.epdfont"

    if [ "$two_bit" = true ]; then
      python fontconvert.py --binary --quiet --2bit $font_name $size $font_path
    else
      python fontconvert.py --binary --quiet $font_name $size $font_path
    fi

    final_path="${FAMILY_NAME}-${style}-${size}.epdfont"
    mv $output_path $final_path

    echo "$final_path généré"
  done
done

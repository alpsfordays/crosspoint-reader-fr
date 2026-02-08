#!/bin/bash

set -e

read -p "Enter the name of the font family: " FAMILY_NAME

while true; do
    read -p "Would you like to compile in 2 bit mode (antialiasing)? [y/n] " yn
    case $yn in
        [Yy]* ) two_bit=true;  break;;
        [Nn]* ) two_bit=false; break;;
        * ) echo "Please answer yes or no.";;
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
      echo "Neither .otf or .ttf file for ${FAMILY_NAME}-${style} found, the program will skip it"
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

    echo "Generated $final_path"
  done
done

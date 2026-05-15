cat << 'EOF' > dump_complete.sh
echo "# Project Structure" > project_context.md
echo '```' >> project_context.md
# Proper tree-like indentation using sed
find . -maxdepth 4 -not -path '*/.*' -not -path './build*' -not -path './vcpkg*' -not -path './vcpkg_installed*' | sed -e 's/[^-][^\/]*\// |/g' -e 's/|\([^ ]\)/|-\1/' >> project_context.md
echo '```' >> project_context.md
echo "" >> project_context.md
echo "# Source and Build Files" >> project_context.md

# Captures C++, Shaders, and CMakeLists
find . -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.vert" -o -name "*.frag" -o -name "CMakeLists.txt" \) \
-not -path '*/.*' -not -path './build*' -not -path './vcpkg*' -not -path './vcpkg_installed*' | while read -r file; do
    echo "## File: $file" >> project_context.md
    ext="${file##*.}"
    filename=$(basename "$file")
    
    if [[ "$filename" == "CMakeLists.txt" ]]; then
        echo '```cmake' >> project_context.md
    elif [[ "$ext" == "vert" || "$ext" == "frag" ]]; then
        echo '```glsl' >> project_context.md
    else
        echo '```cpp' >> project_context.md
    fi
    
    cat "$file" >> project_context.md
    echo '```' >> project_context.md
    echo "" >> project_context.md
done
EOF
bash dump_complete.sh && rm dump_complete.sh
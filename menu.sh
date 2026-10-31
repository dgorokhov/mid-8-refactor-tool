#!/bin/bash

# Configuration
MENU_FILE="menu.txt"

# Check if menu file exists
if [ ! -f "$MENU_FILE" ]; then
    echo "Error: Menu file '$MENU_FILE' not found."
    exit 1
fi

# Read lines into an array
lines=()
while IFS= read -r line || [[ -n "$line" ]]; do
    # Skip empty lines
    [[ -z "$line" ]] && continue
    lines+=("$line")
done < "$MENU_FILE"

# Display the menu (only short names, numbered from 0)
echo "...Menu............"
for i in "${!lines[@]}"; do
    # Extract short name (everything before the first comma)
    short_name="${lines[$i]%%,*}"
    echo "[$i] $short_name"
done
echo "..................."

# Get user input
read -p "Choose an option (number): " choice

# Validate user input
if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -ge "${#lines[@]}" ]; then
    echo "Error: Invalid selection."
    exit 1
fi

# Extract and run the bash command 
# (everything after the first comma)
selected_line="${lines[$choice]}"
command_to_run="${selected_line#*,}"

echo "Running.......$command_to_run\n\n"
#echo "------------------------"
eval "$command_to_run"

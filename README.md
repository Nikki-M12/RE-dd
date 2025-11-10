# REimplementation of the DD command

## How to compile
### Step 1: Install dependencies
### Ubuntu/Debian
```bash
sudo apt install build-essential git
```
### Alpine
```bash
sudo apk add build-base git
```
### Fedora
```bash
sudo dnf install development-tools git
```

## Step 2: Download code

```bash
git clone https://github.com/Nikki-M12/RE-dd
cd REdd
```

## Step 3: Compile this code

```bash
gcc -o REdd REdd.c
```

# Usage
- Basic usage example
```bash
./REdd if=input_file of=output_file
```
- With progress
```bash
./REdd if=input_file of=output_file status=progress
```
- With a specific block size
```bash
./REdd if=input_file of=output_file bs=1M status=progress
```
- To see information about the program
```bash
./REdd --about
```

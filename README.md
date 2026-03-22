# hwmond APT Repository

This branch hosts the Debian package repository for hwmond.

## Setup

```bash
echo "deb [trusted=yes] https://raw.githubusercontent.com/mav2287/hwmond/apt-repo ./" | sudo tee /etc/apt/sources.list.d/hwmond.list
sudo apt update
sudo apt install hwmond-xserve
```

## Updates

```bash
sudo apt update && sudo apt upgrade
```
# Updated: Sun Mar 22 03:38:34 UTC 2026

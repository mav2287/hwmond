Name:           hwmond-xserve
Version:        %{_hwmond_version}
Release:        1
Summary:        Apple Xserve hardware monitor for Linux
License:        MIT
URL:            https://github.com/mav2287/hwmond
BuildArch:      noarch

Requires:       dmidecode
Recommends:     ipmitool
Recommends:     smartmontools
Recommends:     ethtool

%description
Drives front panel CPU activity LEDs and populates BMC for Apple Server
Monitor on Apple Xserve (1,1 / 2,1 / 3,1) servers running Linux.

Blocks installation on non-Xserve hardware to prevent BMC damage.

%install
mkdir -p %{buildroot}/usr/local/sbin
mkdir -p %{buildroot}/etc/systemd/system
mkdir -p %{buildroot}/etc/udev/rules.d
mkdir -p %{buildroot}/etc/modprobe.d
cp %{_hwmond_binary} %{buildroot}/usr/local/sbin/hwmond
chmod 755 %{buildroot}/usr/local/sbin/hwmond
cp %{_hwmond_scripts}/hwmond.service %{buildroot}/etc/systemd/system/hwmond.service
chmod 644 %{buildroot}/etc/systemd/system/hwmond.service
cp %{_hwmond_scripts}/99-xserve-panel.rules %{buildroot}/etc/udev/rules.d/99-xserve-panel.rules
chmod 644 %{buildroot}/etc/udev/rules.d/99-xserve-panel.rules
cp %{_hwmond_scripts}/hwmond-ipmi.conf %{buildroot}/etc/modprobe.d/hwmond-ipmi.conf
chmod 644 %{buildroot}/etc/modprobe.d/hwmond-ipmi.conf

%files
/usr/local/sbin/hwmond
/etc/systemd/system/hwmond.service
/etc/udev/rules.d/99-xserve-panel.rules
/etc/modprobe.d/hwmond-ipmi.conf

%post
systemctl daemon-reload
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true
modprobe ipmi_devintf 2>/dev/null || true
modprobe ipmi_si 2>/dev/null || true
systemctl enable hwmond
systemctl start hwmond

%preun
systemctl stop hwmond 2>/dev/null || true
systemctl disable hwmond 2>/dev/null || true

Summary: A remote console viewer.
Name: vtgrab
Version: 0.1.9
Release: 1
License: GPL
Group: Applications/System
URL: http://cyberelk.net/tim/vtgrab
Source0: http://cyberelk.net/tim/data/vtgrab/stable/vtgrab-%{version}.tar.bz2
BuildRoot: %{_tmppath}/%{name}-%{version}-buildroot

%description
Using vtgrab you can observe and control the console of a remote machine.

%package server
Summary: A console server for vtgrab.
Group: Applications/System
PreReq: ed, initscripts

%description server
This is the RVC server for vtgrab.

%prep
%setup -q -n %{name}-%{version}

%build
%configure
make CFLAGS="$RPM_OPT_FLAGS"

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/sbin
mkdir -p %{buildroot}/%{_bindir}
mkdir -p %{buildroot}/%{_mandir}/man1
install -m 755 twiglet %{buildroot}/%{_bindir}/twiglet
install -m 755 rvc %{buildroot}/%{_bindir}/rvc
install -m 644 twiglet.1 %{buildroot}/%{_mandir}/man1/twiglet.1
install -m 755 rvcd %{buildroot}/sbin/rvcd
xmlto -o doc pdf doc/rvc.sgml

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root)
%doc README COPYING BUGS NEWS doc/rvc doc/rvc.pdf
%{_bindir}/rvc
%{_bindir}/twiglet
%{_mandir}/*/*

%files server
%defattr(-,root,root)
/sbin/rvcd

%post server
# This is specific to Red Hat Linux 7.
# /bin/ed -s /etc/rc.d/rc.sysinit << EOF
# 566,570m182
# 187a
# [ -x /sbin/rvcd ] && ( /sbin/rvcd /dev/ttyS0 > /dev/null 2>&1 & )
# 
# .
# wq
# EOF

%postun server
# This is specific to Red Hat Linux 7.
# /bin/ed -s /etc/rc.d/rc.sysinit << EOF
# 183,187m573
# 183,184d
# wq
# EOF

%changelog
* Wed Jan 21 2004 Tim Waugh <twaugh@redhat.com>
- Distributed as tar.bz2 now.

* Mon Sep 25 2000 Tim Waugh <twaugh@redhat.com>
- Created

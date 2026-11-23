FROM archlinux:latest

# Configure domestic China mirrors, disable seccomp sandbox (for BuildKit/Rosetta containers) & enable multilib
RUN printf 'Server = https://mirrors.aliyun.com/archlinux/$repo/os/$arch\nServer = https://mirrors.tuna.tsinghua.edu.cn/archlinux/$repo/os/$arch\nServer = https://mirrors.ustc.edu.cn/archlinux/$repo/os/$arch\n' > /etc/pacman.d/mirrorlist \
    && sed -i '/^\[options\]/a DisableSandboxSyscalls' /etc/pacman.conf \
    && printf '\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n' >> /etc/pacman.conf \
    && pacman -Sy --noconfirm archlinux-keyring \
    && pacman -Syu --noconfirm \
    && pacman -S --noconfirm --needed \
        archiso \
        base-devel \
        cmake \
        pkgconf \
        gtk4 \
        webkitgtk-6.0 \
        gtk4-layer-shell \
        git \
    && pacman -Scc --noconfirm

WORKDIR /workspace

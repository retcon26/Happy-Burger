TARGET = sandbox
OBJS = src/main.o src/gfx.o src/hud.o src/input.o src/camera.o src/mem.o src/pak.o src/map.o \
       src/collision.o src/player.o src/sky.o src/proplib.o src/propx.o src/physics.o src/game.o \
       src/audio.o src/ents.o src/script.o src/ui.o src/settings.o src/menu.o src/npc.o src/net.o src/vmodel.o \
       src/vehicle.o src/voice.o src/osk.o src/achieve.o src/workshop.o src/builds.o

INCDIR = src
# -MMD -MP: each .o also depends on the headers it includes (the -include at the end), so changing
# one (version.h, settings.h...) rebuilds everything that uses it
CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -ffast-math -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -llua -lpspgum -lpspgu -lpspdebug -lpspctrl -lpsppower -lpspaudio -lm -lpspnet_adhocctl -lpspnet_adhoc -lpspnet_apctl -lpspnet_inet -lpspnet_resolver -lpspnet -lpsputility

EXTRA_TARGETS = EBOOT.PBP
# The XMB's name for it. The SDK puts it in '...' for the shell, so the apostrophe is written '\''
PSP_EBOOT_TITLE = Retcon'\''s Mod
# XMB icon and background, drawn by tools/make_psp_icons.py
PSP_EBOOT_ICON = ICON0.PNG
PSP_EBOOT_PIC1 = PIC1.PNG
# MEMSIZE=1: the long-standing flag for the PSP-2000/3000/Go's extra RAM (52MB instead of 24MB), which
# every firmware and PPSSPP honour. (The SDK's newer default, 2, isn't always recognised.)
PSP_LARGE_MEMORY = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

-include $(OBJS:.o=.d)

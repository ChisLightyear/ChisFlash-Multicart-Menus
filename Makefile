# 原始菜单作者：Lesserkuma；扩展作者：ZaindORp。感谢两位作者的工作。
# 在 devkitPro 环境运行 make，成品位于 .build/ 各菜单目录。
MENUS := LK LK-WL NS NS-Lite
.PHONY: all $(MENUS)
all: $(MENUS)
$(MENUS):
	$(MAKE) -C $@

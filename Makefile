# OpenWrt SDK package for the iw-test-agent DUT agent.
#   cp -rL dut_agent/openwrt <sdk>/package/iw-test-agent
#   make package/iw-test-agent/compile V=s
include $(TOPDIR)/rules.mk

PKG_NAME:=iw-test-agent
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/iw-test-agent
  SECTION:=net
  CATEGORY:=Network
  TITLE:=iw-test-agent Wi-Fi test DUT agent
  DEPENDS:=+iw +iwinfo
endef

define Package/iw-test-agent/description
  Agent that connects to the iw-test-agent controller and runs
  AP/STA (WDS) and 802.11s mesh tests using uci/wifi/iw.
endef

define Package/iw-test-agent/conffiles
/etc/config/iw-test-agent
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/iw-test-agent.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -std=gnu99 \
		-o $(PKG_BUILD_DIR)/iw-test-agent $(PKG_BUILD_DIR)/iw-test-agent.c $(TARGET_LDFLAGS)
endef

define Package/iw-test-agent/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/iw-test-agent $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/iw-test-agent.init $(1)/etc/init.d/iw-test-agent
	$(INSTALL_CONF) ./files/iw-test-agent.config $(1)/etc/config/iw-test-agent
endef

$(eval $(call BuildPackage,iw-test-agent))

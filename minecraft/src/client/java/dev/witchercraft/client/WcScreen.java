package dev.witchercraft.client;

import net.minecraft.ChatFormatting;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.AbstractSelectionList;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.MultiLineTextWidget;
import net.minecraft.client.gui.components.ScrollableLayout;
import net.minecraft.client.gui.components.StringWidget;
import net.minecraft.client.gui.layouts.HeaderAndFooterLayout;
import net.minecraft.client.gui.layouts.LinearLayout;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.network.chat.CommonComponents;
import net.minecraft.network.chat.Component;
import org.jspecify.annotations.Nullable;

/**
 * The WitcherCraft screens' frame: title on top, a scrolling column in the middle (so nothing is
 * cut off at big GUI scales), Done at the bottom. Subclasses fill {@link #addContents}.
 */
abstract class WcScreen extends Screen {
	protected final Screen parent;
	protected HeaderAndFooterLayout layout = new HeaderAndFooterLayout(this);
	private @Nullable ScrollableLayout scroll;

	protected WcScreen(Component title, Screen parent) {
		super(title);
		this.parent = parent;
	}

	/** Column width that fits the window. */
	protected int columnWidth() {
		return Math.min(this.width - 40, 410);
	}

	protected abstract void addContents(LinearLayout column);

	protected void addFooter(LinearLayout footer) {
		footer.addChild(Button.builder(CommonComponents.GUI_DONE, b -> this.onClose()).width(200).build());
	}

	@Override
	protected final void init() {
		this.layout = new HeaderAndFooterLayout(this); // init runs again on rebuildWidgets
		this.layout.addTitleHeader(this.title, this.font);
		LinearLayout column = LinearLayout.vertical().spacing(4);
		column.defaultCellSetting().alignHorizontallyCenter();
		this.addContents(column);
		this.scroll = this.layout.addToContents(new ScrollableLayout(this.minecraft, column, this.layout.getContentHeight()));
		this.addFooter(this.layout.addToFooter(LinearLayout.horizontal().spacing(8)));
		this.layout.visitWidgets(this::addRenderableWidget);
		this.repositionElements();
	}

	@Override
	protected void repositionElements() {
		if (this.scroll == null) {
			return;
		}
		this.scroll.arrangeElements();
		this.scroll.setMaxHeight(this.layout.getContentHeight());
		this.scroll.setMinHeight(this.layout.getContentHeight());
		this.layout.arrangeElements();
		this.scroll.setPosition(this.width / 2 - this.scroll.getWidth() / 2, this.layout.getHeaderHeight());
	}

	// The dark list background and separators of Minecraft's in-world option screens (as vanilla's
	// WorldOptionsScreen), so the hotbar and the Witcher's picture behind don't show through the text.
	@Override
	protected void extractMenuBackground(GuiGraphicsExtractor graphics) {
		super.extractMenuBackground(graphics);
		if (this.scroll == null) {
			return;
		}
		graphics.blit(RenderPipelines.GUI_TEXTURED, AbstractSelectionList.INWORLD_MENU_LIST_BACKGROUND, this.layout.getX(), this.layout.getHeaderHeight(),
			this.width, this.height - this.layout.getFooterHeight() + (int) this.scroll.getScrollAmount(), this.width, this.layout.getContentHeight(), 32, 32);
	}

	@Override
	public void extractRenderState(GuiGraphicsExtractor graphics, int xm, int ym, float a) {
		super.extractRenderState(graphics, xm, ym, a);
		graphics.blit(RenderPipelines.GUI_TEXTURED, Screen.INWORLD_HEADER_SEPARATOR, this.layout.getX(), this.layout.getHeaderHeight() - 2, 0.0F, 0.0F,
			this.width, 2, 32, 2);
		graphics.blit(RenderPipelines.GUI_TEXTURED, Screen.INWORLD_FOOTER_SEPARATOR, this.layout.getX(), this.height - this.layout.getFooterHeight(), 0.0F,
			0.0F, this.width, 2, 32, 2);
	}

	@Override
	public void onClose() {
		this.minecraft.gui.setScreen(this.parent);
	}

	@Override
	public boolean isPauseScreen() {
		return false;
	}

	// ---- building blocks ----

	protected StringWidget heading(String text) {
		return new StringWidget(Component.literal(text).withStyle(ChatFormatting.BOLD, ChatFormatting.UNDERLINE), this.font);
	}

	protected MultiLineTextWidget text(Component text) {
		return new MultiLineTextWidget(text, this.font).setMaxWidth(this.columnWidth());
	}

	protected MultiLineTextWidget text(String text) {
		return this.text(Component.literal(text));
	}
}

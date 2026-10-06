package dev.crashcraft.client;

import dev.crashcraft.combat.SkyCombat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class CrashCraftClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		dev.crashcraft.link.SkyLink.announceRunning();
		// DiscordPresence.start(); (SkyCraft-specific)
		DestructionToggle.register();
		// Multiplayer without editing files: the host opens their world to LAN (O, Open to LAN) and
		// e4mc gives them a link; friends type /join <link> in chat, and /leave to come back.
		net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("join")
				.then(net.fabricmc.fabric.api.client.command.v2.ClientCommands.argument("link", com.mojang.brigadier.arguments.StringArgumentType.greedyString())
					.executes(c -> {
						String link = com.mojang.brigadier.arguments.StringArgumentType.getString(c, "link");
						c.getSource().sendFeedback(net.minecraft.network.chat.Component.literal("Joining " + link.trim() + "..."));
						// After the chat screen has closed: this leaves the current world.
						net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.joinFriend(net.minecraft.client.Minecraft.getInstance(), link));
						return 1;
					})));
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("leave").executes(c -> {
				net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.leaveFriend(net.minecraft.client.Minecraft.getInstance()));
				return 1;
			}));
		});
		ClientTickEvents.END_CLIENT_TICK.register(SkyClient::clientTick);
		// CrashCraft: start behind the player, like Crash's own camera (F5 still cycles the views).
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> {
			if (!Boolean.getBoolean("crashcraft.firstPerson")) {
				minecraft.execute(() -> minecraft.options.setCameraType(net.minecraft.client.CameraType.THIRD_PERSON_BACK));
			}
		});
		// Multiplayer testing on one PC: SKYCRAFT_LAN_PORT opens the world to LAN on that port as soon
		// as it's loaded, and SKYCRAFT_LAN_OFFLINE lets offline (dev) clients join it.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> {
			String port = System.getenv("SKYCRAFT_LAN_PORT");
			var server = minecraft.getSingleplayerServer();
			if (port == null || port.isBlank() || server == null || server.isPublished()) {
				return;
			}
			minecraft.execute(() -> {
				if (System.getenv("SKYCRAFT_LAN_OFFLINE") != null) {
					server.setUsesAuthentication(false);
				}
				boolean ok = server.publishServer(net.minecraft.server.MinecraftServer.MultiplayerScope.LAN, false, Integer.parseInt(port.trim()));
				dev.crashcraft.CrashCraft.LOG.info("CrashCraft: world opened to LAN on port {} ({}{})", port.trim(), ok ? "ok" : "FAILED",
					System.getenv("SKYCRAFT_LAN_OFFLINE") != null ? ", offline logins allowed" : "");
			});
		});
		// A guest in a friend's world: dying there kills this player's own Skyrim character.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.registerGlobalReceiver(dev.crashcraft.net.SkyNet.Died.TYPE, (payload, context) -> {
			if (dev.crashcraft.link.SkyLink.active()) {
				dev.crashcraft.link.SkyLink.pushEvent(dev.crashcraft.link.Proto.EV_PLAYER_DIED, payload.attackerFormId(), 0, 0, 0, 0, 0);
			}
		});
		// Skyrim draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRendererRegistry.register(SkyCombat.SKYRIM_ACTOR, NoopRenderer::new);
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never Skyrim's voxels; otherwise the server sees the smooth position
		// dip into a voxel and teleports the player back every few ticks.
		dev.crashcraft.world.SkyCollision.setSmoothCollider(e -> e instanceof net.minecraft.world.entity.player.Player && SkyClient.linked());
	}
}

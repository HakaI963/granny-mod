.class public Lcom/safedk/android/SafeDKApplication;
.super Landroid/app/Application;
.source "SourceFile"


# static fields
.field private static context:Landroid/content/Context;

.field private static final mHandler:Landroid/os/Handler;


# direct methods
.method static constructor <clinit>()V
    .locals 1

    # --- GrannyMod loader: runs before anything else in the process ---
    # Wrapped so a missing/broken libgrannymod.so can never stop the game.
    :try_start_grannymod
    const-string v0, "grannymod"

    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
    :try_end_grannymod
    .catch Ljava/lang/Throwable; {:try_start_grannymod .. :try_end_grannymod} :catch_grannymod
    goto :goto_grannymod

    :catch_grannymod
    move-exception v0

    const-string v0, "GrannyMod"

    const-string v0, "native loader unavailable"

    invoke-static {v0, v0}, Landroid/util/Log;->e(Ljava/lang/String;Ljava/lang/String;)I

    :goto_grannymod
    # --- end GrannyMod loader ---

    .line 15
    new-instance v0, Landroid/os/Handler;

    invoke-direct {v0}, Landroid/os/Handler;-><init>()V

    sput-object v0, Lcom/safedk/android/SafeDKApplication;->mHandler:Landroid/os/Handler;

    return-void
.end method

.method public constructor <init>()V
    .locals 0

    .line 10
    invoke-direct {p0}, Landroid/app/Application;-><init>()V

    return-void
.end method

.method public static getAppContext()Landroid/content/Context;
    .locals 1

    .line 36
    sget-object v0, Lcom/safedk/android/SafeDKApplication;->context:Landroid/content/Context;

    return-object v0
.end method

.method public static runOnUiThread(Ljava/lang/Runnable;)V
    .locals 3
    .param p0, "runnable"    # Ljava/lang/Runnable;

    .line 41
    const/4 v0, 0x1

    new-array v0, v0, [Ljava/lang/Object;

    const/4 v1, 0x0

    const-string v2, "runOnUiThread started"

    aput-object v2, v0, v1

    const-string v1, "SafeDKApplication"

    invoke-static {v1, v0}, Lcom/safedk/android/utils/Logger;->d(Ljava/lang/String;[Ljava/lang/Object;)I

    .line 42
    invoke-static {}, Ljava/lang/Thread;->currentThread()Ljava/lang/Thread;

    move-result-object v0

    invoke-static {}, Landroid/os/Looper;->getMainLooper()Landroid/os/Looper;

    move-result-object v1

    invoke-virtual {v1}, Landroid/os/Looper;->getThread()Ljava/lang/Thread;

    move-result-object v1

    if-ne v0, v1, :cond_0

    .line 44
    invoke-interface {p0}, Ljava/lang/Runnable;->run()V

    goto :goto_0

    .line 48
    :cond_0
    sget-object v0, Lcom/safedk/android/SafeDKApplication;->mHandler:Landroid/os/Handler;

    invoke-virtual {v0, p0}, Landroid/os/Handler;->post(Ljava/lang/Runnable;)Z

    .line 50
    :goto_0
    return-void
.end method


# virtual methods
.method public onCreate()V
    .locals 3

    .line 19
    invoke-super {p0}, Landroid/app/Application;->onCreate()V

    .line 20
    const/4 v0, 0x1

    new-array v0, v0, [Ljava/lang/Object;

    const/4 v1, 0x0

    const-string v2, "onCreate"

    aput-object v2, v0, v1

    const-string v1, "SafeDKApplication"

    invoke-static {v1, v0}, Lcom/safedk/android/utils/Logger;->d(Ljava/lang/String;[Ljava/lang/Object;)I

    .line 21
    invoke-virtual {p0}, Lcom/safedk/android/SafeDKApplication;->getApplicationContext()Landroid/content/Context;

    move-result-object v0

    sput-object v0, Lcom/safedk/android/SafeDKApplication;->context:Landroid/content/Context;

    .line 23
    invoke-virtual {p0}, Lcom/safedk/android/SafeDKApplication;->getApplicationContext()Landroid/content/Context;

    move-result-object v0

    invoke-static {v0, p0}, Lcom/safedk/android/SafeDK;->a(Landroid/content/Context;Landroid/app/Application;)V

    .line 24
    return-void
.end method

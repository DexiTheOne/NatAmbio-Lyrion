package Plugins::NatAmbioAlpha::Plugin;

use strict;
use warnings;
use base qw(Slim::Plugin::Base);
use File::Basename qw(dirname);
use File::Path qw(make_path);
use File::Spec::Functions qw(catdir catfile);
use File::Temp qw(tempfile);
use JSON::XS qw(decode_json);
use Time::HiRes qw(time);
use Slim::Control::Request;
use Slim::Player::Client;
use Slim::Player::Song;
use Slim::Player::TranscodingHelper;
use Slim::Utils::Misc;
use Slim::Utils::OSDetect;
use Slim::Utils::Prefs;
use Slim::Web::Pages;

my $prefs = preferences('plugin.natambioalpha');
my $page = 'plugins/NatAmbioAlpha/index.html';
my ($root, $helper, $ready, $active);
my %rules;
my %epoch;
my %revision;
my %streams;
my ($originalSongOpen, $songOpenHook);
my $streamSequence = int(time() * 1000);
my %liveAudioControl = map { $_ => 1 } qw(
    bypass nae_bypass xtc_bypass front ambience preamp trim azimuth itd ild alpha slope algorithm
);
my %controls = (
    enabled => [0, 0, 1], reset => [0, 0, 1], bypass => [0, 0, 1],
    nae_bypass => [0, 0, 1], xtc_bypass => [0, 0, 1],
    front => [0, -6, 6], ambience => [0, -6, 6], preamp => [0, -18, 0],
    window => [5, 1, 20], trim => [-3, -12, 6],
    azimuth => [30, 10, 30], itd => [260, 80, 300], ild => [14, 3, 20],
    alpha => [1.8, 1, 2.5], slope => [200, 60, 500], algorithm => [1, 0, 1],
    response => [150, 0, 600],
);

sub getDisplayName { 'PLUGIN_NATAMBIO_ALPHA' }

sub initPlugin {
    my $class = shift;
    $class->SUPER::initPlugin(@_);
    $active = 1;
    $root = catdir(Slim::Utils::OSDetect::dirsFor('cache'), 'NatAmbioAlpha');
    make_path($root, { mode => 0700 }) unless -d $root;
    if (opendir my $dir, $root) {
        for my $file (readdir $dir) {
            unlink catfile($root, $file) if $file =~ /\A[\da-f-]{17}\.\d{1,18}\.timeline\z/;
        }
        closedir $dir;
    }
    $helper = catfile(dirname(__FILE__), 'Bin', 'natambio-alpha');
    # LMS's ZIP installer can remove the executable mode.
    chmod 0755, $helper if -f $helper && !-x $helper;
    $ready = 0;
    if (-x $helper && Slim::Utils::Misc::findbin('sox')
        && open(my $version, '-|', $helper, '--version')) {
        my $banner = <$version> // '';
        $ready = 1 if close($version) && $banner =~ /\A NatAmbio[ ]Alpha[ ]/x;
    }

    Slim::Control::Request::subscribe(\&clientConnected, [['client'], ['new', 'reconnect']]);
    Slim::Control::Request::subscribe(\&playbackStopped, [['playlist'], ['stop']]);
    Slim::Control::Request::addDispatch(['natambioalpha', 'state', '?'], [1, 1, 0, \&getState]);
    Slim::Control::Request::addDispatch(['natambioalpha', 'set', '_key', '_value'], [1, 0, 0, \&setControl]);
    installSongHook() if $ready;
    Slim::Web::Pages->addPageLinks('plugins', { PLUGIN_NATAMBIO_ALPHA => $page });
    Slim::Web::Pages->addPageFunction(qr{^plugins/NatAmbioAlpha/index\.html$}, \&dashboardPage);

    for my $client (Slim::Player::Client::clients()) {
        configurePlayer($client);
    }
}

sub dashboardPage {
    my (undef, undef, undef, undef, $response) = @_;
    $response->header('Cache-Control' => 'no-store');
    # This page is static. Reading it directly avoids serving a compiled
    # Template Toolkit page from an older installed plugin ZIP.
    my $path = catfile(dirname(__FILE__), 'HTML', 'EN', split m{/}, $page);
    if (open my $fh, '<:raw', $path) {
        local $/;
        my $html = <$fh>;
        return \$html if defined $html;
    }
    $response->code(404);
    my $error = 'NatAmbio Alpha dashboard is missing.';
    return \$error;
}

sub shutdownPlugin {
    $active = 0;
    if ($songOpenHook && Slim::Player::Song->can('open') == $songOpenHook) {
        no warnings 'redefine';
        *Slim::Player::Song::open = $originalSongOpen;
    }
    Slim::Control::Request::unsubscribe(\&clientConnected);
    Slim::Control::Request::unsubscribe(\&playbackStopped);
    Slim::Web::Pages->addPageLinks('plugins', { PLUGIN_NATAMBIO_ALPHA => undef });
    for my $id (keys %rules) {
        removeRules($id);
    }
}

sub clientConnected { configurePlayer($_[0]->client) }
sub playbackStopped {
    my $client = $_[0]->client or return;
    my $base = statePath($client) or return;
    return unless -d $root && $ready && settings($client)->{enabled};
    ++$epoch{$base};
    unlink "$base.history", "$base.telemetry";
    unlink "$base.$_.timeline" for @{delete($streams{$base}) || []};
    writeSettings($client, $base);
}

sub statePath {
    my $client = shift or return;
    my $id = lc($client->id);
    return unless $id =~ /\A[\da-f]{2}(?::[\da-f]{2}){5}\z/;
    $id =~ tr/:/-/;
    return catfile($root, $id);
}

sub settings {
    my $client = shift;
    my $cp = $prefs->client($client);
    my %settings;
    for my $key (keys %controls) {
        my $value = $cp->get($key);
        $settings{$key} = defined $value ? $value + 0 : $controls{$key}[0];
    }
    return \%settings;
}

sub writeSettings {
    my ($client, $base) = @_;
    my $settings = settings($client);
    my ($fh, $tmp) = tempfile('settings-XXXXXX', DIR => $root, UNLINK => 0);
    print {$fh} "epoch ", ($epoch{$base} //= int(time() * 1000)), "\n";
    print {$fh} "revision ", ($revision{$base} || 0), "\n";
    for my $key (sort keys %$settings) {
        print {$fh} "$key $settings->{$key}\n";
    }
    return 1 if close($fh) && rename($tmp, "$base.settings");
    unlink $tmp;
    return;
}

sub shellQuote {
    my $value = shift;
    $value =~ s/'/'"'"'/g;
    return "'$value'";
}

sub removeRules {
    my $id = shift;
    my $rules = delete($rules{$id}) || {};
    while (my ($key, $old) = each %$rules) {
        next unless ($Slim::Player::TranscodingHelper::commandTable{$key} // '') eq $old->[0];
        if (defined $old->[1]) {
            $Slim::Player::TranscodingHelper::commandTable{$key} = $old->[1];
            $Slim::Player::TranscodingHelper::capabilities{$key} = $old->[2];
        } else {
            delete $Slim::Player::TranscodingHelper::commandTable{$key};
            delete $Slim::Player::TranscodingHelper::capabilities{$key};
        }
    }
}

sub configurePlayer {
    my $client = shift or return;
    my $base = statePath($client) or return;
    my $id = lc($client->id);
    removeRules($id);
    return unless $active && $ready && settings($client)->{enabled};
    return unless writeSettings($client, $base);

    # The command arguments are filled in by Lyrion after it chooses this player's profile.
    # Encode FLAC for FLAC-capable hardware and honor a player's negotiated rate limit.
    for my $source (qw(flc wav aif mp3 ogg sof)) {
        my $key = "$source-flc-*-$id";
        # SpotOn sends raw PCM on stdin and handles seeking in its daemon.
        my $spotOn = $source eq 'sof';
        my $input = $spotOn
            ? '-t raw --encoding signed-integer --bits $SAMPLESIZE$ --endian little -r 44100 -c 2 -'
            : '$FILE$';
        my $trim = $spotOn ? '' : ' $START$';
        my $seek = $spotOn ? ' $SEEK$' : $trim;
        my $output = $spotOn ? '-t flac -C 0 -b 16 -D -' : '-t flac -C 0 -';
        my $command = "[sox] -q $input -t au -b 24 -$trim" . ' $RESAMPLE$ | '
            . shellQuote($helper) . ' ' . shellQuote($base)
            . "$seek --stream=0 | [sox] -q -t au - $output";
        my $old = $Slim::Player::TranscodingHelper::commandTable{$key};
        my $oldCaps = $Slim::Player::TranscodingHelper::capabilities{$key};
        $rules{$id}{$key} = [$command, $old, $oldCaps];
        $Slim::Player::TranscodingHelper::commandTable{$key} = $command;
        $Slim::Player::TranscodingHelper::capabilities{$key} = $spotOn ? {
            I => 'noArgs', D => 'RESAMPLE=rate %d', E => { NOSTART => 'I' },
        } : {
            F => 'noArgs', T => 'START=trim %s', U => 'START=trim %s =%u',
            D => 'RESAMPLE=rate %d', E => {},
        };
    }
}

sub installSongHook {
    $originalSongOpen = Slim::Player::Song->can('open');
    my $open = $originalSongOpen;
    $songOpenHook = sub {
        my $song = $_[0];
        my $client = $active && $song->master;
        my $rules = $client && $rules{lc($client->id)};
        return $open->(@_) unless $rules;

        # Lyrion creates a separate conversion command per song open, including
        # seeks and gapless prefetch. Scope our stream identifier to that call.
        my $stream = ++$streamSequence;
        local %Slim::Player::TranscodingHelper::commandTable =
            %Slim::Player::TranscodingHelper::commandTable;
        my $seek = ($song->seekdata || $song->startOffset) ? ' --seek' : '';
        for my $key (keys %$rules) {
            my $command = \$Slim::Player::TranscodingHelper::commandTable{$key};
            next unless $$command && $$command eq $rules->{$key}[0];
            $$command =~ s/--stream=0/--stream=$stream/;
            $$command =~ s/ \$SEEK\$/$seek/;
        }
        my @result = $open->(@_);
        if ($result[0]) {
            $song->pluginData('natambioalpha.stream', $stream);
            my $base = statePath($client);
            my $entries = $streams{$base} ||= [];
            push @$entries, $stream;
            while (@$entries > 4) {
                my $expired = shift @$entries;
                unlink "$base.$expired.timeline";
            }
        }
        return wantarray ? @result : $result[-1];
    };
    no warnings 'redefine';
    *Slim::Player::Song::open = $songOpenHook;
}

sub timelineRecord {
    my ($fh, $index) = @_;
    return unless seek($fh, $index * 512, 0);
    return unless read($fh, my $raw, 512) == 512;
    my $record = eval { decode_json($raw) };
    return ref $record eq 'HASH' ? $record : undef;
}

sub playbackTelemetry {
    my ($client, $base) = @_;
    my $controller = $client->controller or return;
    return unless $controller->isPlaying(1) || $controller->isPaused;
    my $song = $controller->playingSong or return;
    my $stream = $song->pluginData('natambioalpha.stream') or return;
    return unless open my $fh, '<:raw', "$base.$stream.timeline";
    my $latest = timelineRecord($fh, 0) or return;
    my $rate = $latest->{rate} or return;
    my $start = $song->startOffset || 0;
    my $elapsed = ($controller->playingSongElapsed || 0) - $start;
    my $frames = int(($elapsed > 0 ? $elapsed : 0) * $rate);
    my $interval = int(($rate + 29) / 30);
    my $slot = int($frames / $interval);
    $slot = $latest->{slot} if $slot > $latest->{slot};
    my $telemetry = timelineRecord($fh, 1 + ($slot % 5400));
    if ($telemetry && $telemetry->{frames} > $frames && $slot > 0) {
        --$slot;
        $telemetry = timelineRecord($fh, 1 + ($slot % 5400));
    }
    return unless $telemetry && $telemetry->{slot} == $slot;
    $telemetry->{bufferAhead} = $latest->{frames} > $frames ? ($latest->{frames} - $frames) / $rate : 0;
    $telemetry->{settingsPending} = ($revision{$base} || 0) != $telemetry->{revision} ? 1 : 0;
    if (!$telemetry->{settingsPending} && $telemetry->{revision} && defined $telemetry->{appliedFrame}) {
        $telemetry->{settingsAppliedAt} = $start + $telemetry->{appliedFrame} / $rate;
    }
    # Cached dashboards from before playhead alignment expect a freshness field.
    $telemetry->{age} = 0;
    return $telemetry;
}

sub getState {
    my $request = shift;
    my $client = $request->client;
    my $base = $active && statePath($client);
    unless ($base) { $request->setStatusBadParams(); return; }
    $request->addResult('settings', settings($client));
    $request->addResult('available', $ready);
    my $telemetry = playbackTelemetry($client, $base);
    $request->addResult('telemetry', $telemetry) if $telemetry;
    $request->setStatusDone();
}

sub refreshPlayback {
    my ($client, $base) = @_;
    return if $client->isSynced;
    my $controller = $client->controller or return;
    my $buffering = $controller->buffering;
    return unless $controller->isPlaying(1) || $controller->isPaused
        || ($buffering && ($controller->isStreaming || $controller->isStreamout));
    my $song = $controller->playingSong or return;
    my $stream = $song->pluginData('natambioalpha.stream') or return;
    # The song hook assigns IDs before we know which conversion path Lyrion used.
    return unless -s "$base.$stream.timeline";
    my $duration = $song->duration;
    return unless $duration && $song->canSeek;
    my $position = $buffering ? ($song->startOffset || 0) : $controller->playingSongElapsed;
    return unless defined $position && $position >= 0 && $position < $duration;

    # Lyrion closes the old stream and clears queued audio as part of a seek.
    $controller->jumpToTime($position);
}

sub setControl {
    my $request = shift;
    my $client = $request->client;
    my $base = $active && statePath($client);
    my $key = $request->getParam('_key');
    my $value = $request->getParam('_value');
    unless ($base && $ready && defined $key && exists $controls{$key}
            && defined $value && $value =~ /\A-?\d{1,4}(?:\.\d{1,3})?\z/
            && $value >= $controls{$key}[1] && $value <= $controls{$key}[2]) {
        $request->setStatusBadParams();
        return;
    }
    my $cp = $prefs->client($client);
    my $previous = $cp->get($key);
    my $audioChanged = $liveAudioControl{$key} && $value != ($previous // $controls{$key}[0]);
    ++$revision{$base} if $audioChanged;
    $cp->set($key, $value + 0);
    if ($key eq 'enabled' || $key eq 'reset') {
        ++$epoch{$base};
        unlink "$base.history", "$base.telemetry";
    }
    unless (writeSettings($client, $base)) {
        --$revision{$base} if $audioChanged;
        $cp->set($key, $previous);
        $request->setStatusBadConfig();
        return;
    }
    configurePlayer($client) if $key eq 'enabled';
    refreshPlayback($client, $base) if $audioChanged && settings($client)->{enabled};
    $request->setStatusDone();
}

1;

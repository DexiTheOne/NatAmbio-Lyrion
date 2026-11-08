package Plugins::NatAmbioAlpha::Plugin;

use strict;
use warnings;
use base qw(Slim::Plugin::Base);
use File::Basename qw(dirname);
use File::Path qw(make_path);
use File::Spec::Functions qw(catdir catfile);
use File::Temp qw(tempfile);
use JSON::XS qw(decode_json);
use Time::HiRes qw(stat time);
use Slim::Control::Request;
use Slim::Player::Client;
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
    Slim::Web::Pages->addPageLinks('plugins', { PLUGIN_NATAMBIO_ALPHA => $page });
    Slim::Web::Pages->addPageFunction(qr{^plugins/NatAmbioAlpha/index\.html$}, sub {
        return Slim::Web::HTTP::filltemplatefile($page, $_[1]);
    });

    for my $client (Slim::Player::Client::clients()) {
        configurePlayer($client);
    }
}

sub shutdownPlugin {
    $active = 0;
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
    for my $source (qw(flc wav aif mp3 ogg)) {
        my $key = "$source-flc-*-$id";
        my $command = '[sox] -q $FILE$ -t au -b 24 - $START$ $RESAMPLE$ | '
            . shellQuote($helper) . ' ' . shellQuote($base)
            . ' $START$ | [sox] -q -t au - -t flac -C 0 -';
        my $old = $Slim::Player::TranscodingHelper::commandTable{$key};
        my $oldCaps = $Slim::Player::TranscodingHelper::capabilities{$key};
        $rules{$id}{$key} = [$command, $old, $oldCaps];
        $Slim::Player::TranscodingHelper::commandTable{$key} = $command;
        $Slim::Player::TranscodingHelper::capabilities{$key} = {
            F => 'noArgs', T => 'START=trim %s', U => 'START=trim %s =%u',
            D => 'RESAMPLE=rate %d', E => {},
        };
    }
}

sub getState {
    my $request = shift;
    my $client = $request->client;
    my $base = $active && statePath($client);
    unless ($base) { $request->setStatusBadParams(); return; }
    $request->addResult('settings', settings($client));
    $request->addResult('available', $ready);
    if (open my $fh, '<', "$base.telemetry") {
        my $raw = <$fh>;
        my $modified = (stat $fh)[9];
        close $fh;
        my $telemetry = eval { decode_json($raw // '') };
        if (ref $telemetry eq 'HASH') {
            $telemetry->{age} = $modified ? time() - $modified : 0;
            $request->addResult('telemetry', $telemetry);
        }
    }
    $request->setStatusDone();
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
    $cp->set($key, $value + 0);
    if ($key eq 'enabled' || $key eq 'reset') {
        ++$epoch{$base};
        unlink "$base.history", "$base.telemetry";
    }
    writeSettings($client, $base);
    configurePlayer($client) if $key eq 'enabled';
    $request->setStatusDone();
}

1;

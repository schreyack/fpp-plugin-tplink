
<?
function returnIfExists($json, $setting) {
    if ($json == null) {
        return "";
    }
    if (array_key_exists($setting, $json)) {
        return $json[$setting];
    }
    return "";
}

function convertAndGetSettings() {
    global $settings;
        
    $cfgFile = $settings['configDirectory'] . "/plugin.tplink.json";
    if (file_exists($cfgFile)) {
        $j = file_get_contents($cfgFile);
        $json = json_decode($j, true);
        return $json;
    }
    $j = "[]";
    return json_decode($j, true);
}

$pluginJson = convertAndGetSettings();
?>


<div id="global" class="settings">
<fieldset>
<legend>FPP TP-Link Config</legend>

<script>

var tplinkConfig = <? echo json_encode($pluginJson, JSON_PRETTY_PRINT); ?>;


var uniqueId = 1;
var modelOptions = "";
function AddTPLinkItem(type) {
    var id = $("#tplinkTableBody > tr").length + 1;
    var html = "<tr class='fppTableRow";
    if (id % 2 != 0) {
        html += " oddRow'";
    }
    html += "'><td class='colNumber rowNumber'>" + id + ".</td><td><span style='display: none;' class='uniqueId'>" + uniqueId + "</span></td>";
    html += "<td><input type='text' size='24' class='devname' placeholder='Name' /></td>";
    html += "<td><input type='text' minlength='7' maxlength='15' size='15' pattern='^((\d{1,2}|1\d\d|2[0-4]\d|25[0-5])\.){3}(\d{1,2}|1\d\d|2[0-4]\d|25[0-5])$' class='ipaddress' /></td>";
    html += "<td><input type='number' value='0' min='0' max='10000000' class='startchan' />";
    html += "<td><select class='devicetype'>";
    html += "<option value='light'";
    if(type == 'light') {html += " selected ";}
    html += ">TPLink Light</option><option value='switch'";
    if(type == 'switch') {html += " selected ";}
    html += ">TPLink Switch/Plug</option><option value='goveelight'";
    if(type == 'goveelight') {html += " selected ";}
    html += ">Govee Light</option><option value='tasmotalight'";
    if(type == 'tasmotalight') {html += " selected ";}
    html += ">Tasmota Light</option><option value='tasmotaswitch'";
    if(type == 'tasmotaswitch') {html += " selected ";}
    html += ">Tasmota Switch</option><option value='tapolight'";
    if(type == 'tapolight') {html += " selected ";}
    html += ">Tapo Light</option><option value='taposwitch'";
    if(type == 'taposwitch') {html += " selected ";}
    html += ">Tapo Switch</option></select>";
    var plugDisplay = (type == 'switch' || type == 'tasmotaswitch' || type == 'taposwitch') ? '' : 'display: none;';
    html += "<td><input type='number' value='0' min='0' max='10' class='plugnum' style='" + plugDisplay + "' />";
    var tapoDisplay = (type == 'tapolight' || type == 'taposwitch') ? '' : 'display: none;';
    html += "<td><input type='text' size='20' class='username' placeholder='Email' style='" + tapoDisplay + "' />";
    html += "<td><input type='password' size='20' class='password' placeholder='Password' style='" + tapoDisplay + "' />";
    html += "</tr>";
    //selected
    $("#tplinkTableBody").append(html);

    newRow = $('#tplinkTableBody > tr').last();
    $('#tplinkTableBody > tr').removeClass('selectedEntry');
    DisableButtonClass('deleteEventButton');
    uniqueId++;

    return newRow;
}

function SaveTPLinkItem(row) {
    var ip = $(row).find('.ipaddress').val();
    var startchan = parseInt($(row).find('.startchan').val(),10);

    var plugnum = parseInt($(row).find('.plugnum').val(),10);
	var devicetype = $(row).find('.devicetype').val();

    var username = $(row).find('.username').val();
    var password = $(row).find('.password').val();

    // Start from the entry as it was loaded, so fields this page does not
    // show (DMXLive Studio's "enabled", for one) are kept.
    var json = $.extend({}, $(row).data('orig') || {});
    json["name"] = $.trim($(row).find('.devname').val());
    json["ip"] = ip;
    json["startchannel"] = startchan;
    json["devicetype"] = devicetype;
    json["plugnumber"] = plugnum;
    json["username"] = username;
    json["password"] = password;
    return json;
}

function SaveTPLinkItems() {
    var tplinkConfig = [];
    var i = 0;
    $("#tplinkTableBody > tr").each(function() {
        tplinkConfig[i++] = SaveTPLinkItem(this);
    });

    // Commands and playlists find a device by its name, so every device
    // needs one, no two may share one, and a comma would split it in two.
    var problems = [];
    var seen = {};
    $.each(tplinkConfig, function(idx, item) {
        var n = item["name"];
        if (n == "") {
            problems.push("Row " + (idx + 1) + " has no name.");
            return;
        }
        if (n.indexOf(",") >= 0) {
            problems.push("'" + n + "' has a comma in it.");
        }
        var key = n.toLowerCase();
        if (seen[key]) {
            problems.push("'" + n + "' is used more than once.");
        }
        seen[key] = true;
    });
    $('#tplinkProblems').text(problems.join(" ")).toggle(problems.length > 0);
    if (problems.length > 0) {
        return;
    }
    
    var data = JSON.stringify(tplinkConfig);
    $.ajax({
        type: "POST",
	url: 'api/configfile/plugin.tplink.json',
        dataType: 'json',
        async: false,
        data: data,
        processData: false,
        contentType: 'application/json',
        success: function (data) {
           SetRestartFlag(2);
        }
    });
}


function RenumberRows() {
    var id = 1;
    $('#tplinkTableBody > tr').each(function() {
        $(this).find('.rowNumber').html('' + id++ + '.');
        $(this).removeClass('oddRow');

        if (id % 2 != 0) {
            $(this).addClass('oddRow');
        }
    });
}
function RemoveTPLinkItem() {
    if ($('#tplinkTableBody').find('.selectedEntry').length) {
        $('#tplinkTableBody').find('.selectedEntry').remove();
        RenumberRows();
    }
    DisableButtonClass('deleteEventButton');
}


$(document).ready(function() {
                  
    $('#tplinkTableBody').sortable({
        update: function(event, ui) {
            RenumberRows();
        },
        item: '> tr',
        scroll: true
    }).disableSelection();

    $('#tplinkTableBody').on('mousedown', 'tr', function(event,ui){
        $('#tplinkTableBody tr').removeClass('selectedEntry');
        $(this).addClass('selectedEntry');
        EnableButtonClass('deleteEventButton');
    });

    $('#tplinkTableBody').on('change', '.devicetype', function() {
        var row = $(this).closest('tr');
        var val = $(this).val();
        var isTapo = (val == 'tapolight' || val == 'taposwitch');
        var isPlug = (val == 'switch' || val == 'tasmotaswitch' || val == 'taposwitch');
        row.find('.username').toggle(isTapo);
        row.find('.password').toggle(isTapo);
        row.find('.plugnum').toggle(isPlug);
    });
});

</script>
<div>
<table border=0>
<tr><td colspan='2'>
        <input type="button" value="Save" class="buttons genericButton" onclick="SaveTPLinkItems();">
        <input type="button" value="Add" class="buttons genericButton" onclick="AddTPLinkItem('light');">
        <input id="delButton" type="button" value="Delete" class="deleteEventButton disableButtons genericButton" onclick="RemoveTPLinkItem();">
    </td>
</tr>
</table>

<div class='fppTableWrapper fppTableWrapperAsTable'>
<div class='fppTableContents'>
<table class="fppSelectableRowTable" id="tplinkTable"  width='100%'>
<thead><tr class="fppTableHeader"><th>#</th><th></th><th>Name</th><th>IP</th><th>Start Chan</th><th>Device Type</th><th>Plug ID</th><th>Username</th><th>Password</th></tr></thead>
<tbody id='tplinkTableBody'>
</tbody>
</table>
</div>

</div>
<div id='tplinkProblems' style='display: none; color: #c00; font-weight: bold;'></div>
<div>
<p><b>Plugs and switches in a sequence:</b> a start channel means the sequence owns the plug. When a playlist starts, the plug is sent the channel's state on the first frame. After that the plugin sends only when the state changes (127 or more is on, below 127 is off). Leave the start channel at 0 for plugs you control with commands (playlist lead-ins, Home Assistant).</p>
<p><b>Names:</b> commands and playlists pick a device by its name, so give every device a unique name. The IP is only used here.</p>
</div>
</div>
<script>

$.each(tplinkConfig, function( key, val ) {
    if (val["devicetype"] == "effectchannels") {
        return;  // an old file's entry: not a device, and saving drops it
    }
    var row = AddTPLinkItem(val["devicetype"]);
    $(row).data('orig', val);
    $(row).find('.devname').val(val["name"]);
    $(row).find('.ipaddress').val(val["ip"]);
    $(row).find('.startchan').val(val["startchannel"]);
    $(row).find('.plugnum').val(val["plugnumber"]);
    $(row).find('.username').val(val["username"]);
    $(row).find('.password').val(val["password"]);
});
</script>

</fieldset>
</div>
